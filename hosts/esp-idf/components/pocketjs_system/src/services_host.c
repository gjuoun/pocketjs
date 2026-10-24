/*
 * engine/host-services on ESP-IDF (host_services.h), and the store and app
 * lifecycle of pocketjs/system.h.
 *
 * A slot holds one committed package, `package.pocket`; its hash is the
 * file's footer. An install streams into `link-upload.pocket`, is admitted
 * (footer, FNV-1a64, a variant for the firmware's contract) and replaces
 * `package.pocket`. FAT refuses a rename onto an existing name, so the old
 * package is removed first: an interrupted install loses the app, never
 * leaves a package the firmware would refuse.
 *
 * Storage on the flash partition and the radio's NVS are flash, and a flash
 * access disables the cache, which a task with its stack in PSRAM cannot
 * survive. The guest's runner and the firmware's app task have PSRAM
 * stacks, so every system operation runs on one task with an internal stack
 * while its caller waits (system_run).
 */

#include "pocketjs/system.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fs.h"
#include "host_services.h"
#include "jscan.h"
#include "launcher.h"
#include "link.h"
#include "sha256.h"
#include "system_internal.h"

#define PACKAGE_NAME "package.pocket"
#define STAGING_NAME "link-upload.pocket"
#define SLOT_PATH_BYTES 128
#define SYSTEM_TASK_STACK (12 * 1024)

static const char *TAG = "pocketjs_system";

struct pocketjs_system_app {
  /* A copy in PSRAM for installed apps; NULL for the embedded launcher. */
  uint8_t *owned;
  const uint8_t *bytes;
  size_t size;
  pocketjs_package_t *package;
  pocketjs_package_variant_t variant;
  char slot[17];
  bool launcher;
};

static bool started;
static pocketjs_system_config_t config;
static char root_dir[64];
static char apps_dir[80];
static char link_dir[80];
static char launcher_slot[17];

// ---------------------------------------------------------------------------
// the system task
// ---------------------------------------------------------------------------

static TaskHandle_t system_task;
static SemaphoreHandle_t system_busy;
static SemaphoreHandle_t system_job_ready;
static SemaphoreHandle_t system_job_done;
static void (*system_job)(void *);
static void *system_job_context;

static void system_main(void *unused) {
  (void)unused;
  for (;;) {
    xSemaphoreTake(system_job_ready, portMAX_DELAY);
    system_job(system_job_context);
    xSemaphoreGive(system_job_done);
  }
}

static bool system_task_start(void) {
  if (system_task != NULL) return true;
  system_busy = xSemaphoreCreateMutex();
  system_job_ready = xSemaphoreCreateBinary();
  system_job_done = xSemaphoreCreateBinary();
  /* xTaskCreate takes the stack from internal RAM. */
  return system_busy != NULL && system_job_ready != NULL && system_job_done != NULL &&
         xTaskCreate(system_main, "pocketjs_sys", SYSTEM_TASK_STACK, NULL, 6, &system_task) == pdPASS;
}

void system_run(void (*job)(void *), void *context) {
  if (system_task == NULL || xTaskGetCurrentTaskHandle() == system_task) {
    job(context);
    return;
  }
  xSemaphoreTake(system_busy, portMAX_DELAY);
  system_job = job;
  system_job_context = context;
  xSemaphoreGive(system_job_ready);
  xSemaphoreTake(system_job_done, portMAX_DELAY);
  xSemaphoreGive(system_busy);
}

// ---------------------------------------------------------------------------
// packages
// ---------------------------------------------------------------------------

static uint64_t footer_of(const uint8_t *bytes, size_t size) {
  uint64_t hash = 0;
  if (size < 8) return 0;
  for (int i = 7; i >= 0; i -= 1) hash = (hash << 8) | bytes[size - 8 + (size_t)i];
  return hash;
}

/* Open `bytes` and select the variant the firmware runs. */
static bool open_package(const uint8_t *bytes, size_t size, pocketjs_package_t **package,
                         pocketjs_package_variant_t *variant, char *error, size_t capacity) {
  *package = NULL;
  esp_err_t result = pocketjs_package_open(bytes, size, 0, package);
  if (result != ESP_OK) {
    snprintf(error, capacity, "the package is damaged (%s)", esp_err_to_name(result));
    return false;
  }
  variant->struct_size = sizeof *variant;
  result = pocketjs_package_select(*package, config.contract, variant);
  if (result != ESP_OK) {
    snprintf(error, capacity, "the package has no variant for %s (%s)", config.contract->target_id,
             esp_err_to_name(result));
    pocketjs_package_close(*package);
    *package = NULL;
    return false;
  }
  return true;
}

static LauncherPackage facts_of(const pocketjs_package_variant_t *variant, const uint8_t *bytes, size_t size) {
  return (LauncherPackage){
    .plan = (const char *)variant->plan_json.data,
    .plan_length = variant->plan_json.size,
    .hash = footer_of(bytes, size),
    .bytes = size,
  };
}

/* The whole file in PSRAM, 16-byte aligned like an embedded package. */
static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return NULL;
  uint8_t *bytes = NULL;
  long length = -1;
  if (fseek(file, 0, SEEK_END) == 0) length = ftell(file);
  if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
    bytes = heap_caps_aligned_alloc(16, (size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bytes != NULL && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
      heap_caps_free(bytes);
      bytes = NULL;
    }
  }
  fclose(file);
  if (bytes != NULL) *size = (size_t)length;
  return bytes;
}

typedef struct {
  uint8_t *bytes;
  pocketjs_package_t *package;
} HeldPackage;

static bool load_path(const char *path, uint64_t expected, LauncherPackage *out, void **held, char *error,
                      size_t capacity) {
  size_t size = 0;
  uint8_t *bytes = read_file(path, &size);
  if (bytes == NULL) {
    snprintf(error, capacity, "the package cannot be read");
    return false;
  }
  if (expected != 0 && footer_of(bytes, size) != expected) {
    heap_caps_free(bytes);
    snprintf(error, capacity, "the package does not match its declared hash");
    return false;
  }
  pocketjs_package_t *package = NULL;
  pocketjs_package_variant_t variant = {0};
  HeldPackage *keep = malloc(sizeof *keep);
  if (keep == NULL || !open_package(bytes, size, &package, &variant, error, capacity)) {
    if (keep == NULL) snprintf(error, capacity, "out of memory");
    free(keep);
    heap_caps_free(bytes);
    return false;
  }
  keep->bytes = bytes;
  keep->package = package;
  *out = facts_of(&variant, bytes, size);
  *held = keep;
  return true;
}

static bool plan_feature(const pocketjs_package_variant_t *variant, const char *feature) {
  JsonSpan plan = json_span((const char *)variant->plan_json.data, variant->plan_json.size);
  JsonSpan features;
  JsonSpan value;
  return json_find(plan, "features", &features) && json_is_object(features) &&
         json_find(features, feature, &value) && value.length == 4 && memcmp(value.start, "true", 4) == 0;
}

// ---------------------------------------------------------------------------
// host_services.h: clocks and randomness
// ---------------------------------------------------------------------------

uint64_t services_host_ms(void) {
  return (uint64_t)esp_timer_get_time() / 1000u;
}

uint64_t services_host_us(void) {
  return (uint64_t)esp_timer_get_time();
}

uint64_t services_host_epoch_ms(void) {
  struct timeval now;
  gettimeofday(&now, NULL);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u;
}

void services_host_random(uint8_t *out, size_t length) {
  esp_fill_random(out, length);
}

// ---------------------------------------------------------------------------
// host_services.h: packages
// ---------------------------------------------------------------------------

const char *launcher_host_apps_directory(void) {
  return apps_dir;
}

bool launcher_host_slot_path(const char *slot, const char *name, char *out, size_t capacity) {
  if (slot == NULL || strlen(slot) != 16) return false;
  int written = name == NULL ? snprintf(out, capacity, "%s/%s", apps_dir, slot)
                             : snprintf(out, capacity, "%s/%s/%s", apps_dir, slot, name);
  return written > 0 && (size_t)written < capacity;
}

bool launcher_host_active_hash(const char *slot, uint64_t *hash) {
  char path[SLOT_PATH_BYTES];
  if (!launcher_host_slot_path(slot, PACKAGE_NAME, path, sizeof path)) return false;
  FILE *file = fopen(path, "rb");
  if (file == NULL) return false;
  uint8_t footer[8];
  bool ok = fseek(file, -8, SEEK_END) == 0 && fread(footer, 1, sizeof footer, file) == sizeof footer;
  fclose(file);
  if (ok) *hash = footer_of(footer, sizeof footer);
  return ok;
}

bool launcher_host_load(const char *slot, uint64_t hash, LauncherPackage *out, void **held) {
  char path[SLOT_PATH_BYTES];
  char error[160];
  if (!launcher_host_slot_path(slot, PACKAGE_NAME, path, sizeof path)) return false;
  if (load_path(path, hash, out, held, error, sizeof error)) return true;
  ESP_LOGW(TAG, "slot %s: %s", slot, error);
  return false;
}

void services_host_release(void *held) {
  HeldPackage *keep = held;
  if (keep == NULL) return;
  pocketjs_package_close(keep->package);
  heap_caps_free(keep->bytes);
  free(keep);
}

// ---------------------------------------------------------------------------
// host_services.h: the Pocket Link
// ---------------------------------------------------------------------------

const char *link_host_target(void) {
  return config.contract->target_id;
}

void link_host_identity(char *name, size_t name_capacity, char *model, size_t model_capacity) {
  snprintf(name, name_capacity, "%s", config.device_name != NULL ? config.device_name : "ESP-IDF");
  snprintf(model, model_capacity, "%s", config.device_model != NULL ? config.device_model : "ESP-IDF");
}

const char *link_host_directory(void) {
  return link_dir;
}

bool link_host_net_ready(void) {
  return wifi_online();
}

void link_host_address(char *out, size_t capacity) {
  wifi_address(out, capacity);
}

bool link_host_staging_path(const char *slot, char *out, size_t capacity) {
  char directory[SLOT_PATH_BYTES];
  if (!launcher_host_slot_path(slot, NULL, directory, sizeof directory)) return false;
  mkdir(apps_dir, 0777);
  mkdir(directory, 0777);
  return launcher_host_slot_path(slot, STAGING_NAME, out, capacity);
}

bool link_host_install(const char *slot, const char *path, uint64_t hash, LauncherPackage *out, void **held,
                       char *error, size_t error_capacity) {
  if (!load_path(path, hash, out, held, error, error_capacity)) {
    remove(path);
    return false;
  }
  char committed[SLOT_PATH_BYTES];
  if (!launcher_host_slot_path(slot, PACKAGE_NAME, committed, sizeof committed)) {
    services_host_release(*held);
    snprintf(error, error_capacity, "install: bad slot");
    return false;
  }
  remove(committed);
  if (rename(path, committed) != 0) {
    services_host_release(*held);
    snprintf(error, error_capacity, "install: the package could not be committed");
    return false;
  }
  return true;
}

bool link_host_remove(const char *slot) {
  char directory[SLOT_PATH_BYTES];
  struct stat info;
  if (!launcher_host_slot_path(slot, NULL, directory, sizeof directory) || stat(directory, &info) != 0) {
    return false;
  }
  return fs_remove_tree(directory, FS_MAX_DEPTH + 2);
}

void link_host_linked(bool up) {
  (void)up;
}

bool link_host_ctrl(const char *json, size_t length) {
  (void)json;
  (void)length;
  return false;
}

bool link_host_data(uint32_t stream, const uint8_t *bytes, size_t length) {
  (void)stream;
  (void)bytes;
  (void)length;
  return false;
}

// ---------------------------------------------------------------------------
// pocketjs/system.h
// ---------------------------------------------------------------------------

typedef struct {
  const char *label;
  const char *root;
  esp_err_t result;
} FlashMount;

static void mount_flash(void *argument) {
  FlashMount *mount = argument;
  const esp_vfs_fat_mount_config_t options = {
      .format_if_mount_failed = true,
      .max_files = 8,
      .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
  };
  wl_handle_t handle = WL_INVALID_HANDLE;
  mount->result = esp_vfs_fat_spiflash_mount_rw_wl(mount->root, mount->label, &options, &handle);
}

esp_err_t pocketjs_system_mount_flash(const char *label, const char *root) {
  if (!system_task_start()) return ESP_ERR_NO_MEM;
  FlashMount mount = { .label = label, .root = root, .result = ESP_FAIL };
  system_run(mount_flash, &mount);
  return mount.result;
}

/* The store's directories, the launcher's slot and the link's identity. */
static void start_store(void *argument) {
  esp_err_t *result = argument;
  mkdir(apps_dir, 0777);
  pocketjs_package_t *package = NULL;
  pocketjs_package_variant_t variant = {0};
  char error[160];
  const uint8_t *bytes = config.launcher;
  if (!open_package(bytes, config.launcher_size, &package, &variant, error, sizeof error)) {
    ESP_LOGE(TAG, "launcher: %s", error);
    *result = ESP_ERR_INVALID_ARG;
    return;
  }
  LauncherPackage facts = facts_of(&variant, bytes, config.launcher_size);
  char id[LAUNCHER_ID_BYTES];
  if (!launcher_package_app_id(&facts, id, sizeof id)) {
    pocketjs_package_close(package);
    *result = ESP_ERR_INVALID_ARG;
    return;
  }
  sha256_slot(id, strlen(id), launcher_slot);
  launcher_init(launcher_slot, &facts);
  pocketjs_package_close(package);
  link_init();
}

esp_err_t pocketjs_system_start(const pocketjs_system_config_t *settings) {
  if (started) return ESP_ERR_INVALID_STATE;
  if (settings == NULL || settings->struct_size < sizeof *settings || settings->contract == NULL ||
      settings->launcher == NULL || settings->root == NULL || strlen(settings->root) >= sizeof root_dir - 8) {
    return ESP_ERR_INVALID_ARG;
  }
  config = *settings;
  snprintf(root_dir, sizeof root_dir, "%s", settings->root);
  snprintf(apps_dir, sizeof apps_dir, "%s/apps", root_dir);
  snprintf(link_dir, sizeof link_dir, "%s/link", root_dir);
  if (!system_task_start()) return ESP_ERR_NO_MEM;
  esp_err_t result = ESP_OK;
  system_run(start_store, &result);
  if (result != ESP_OK) return result;
  if (settings->wifi) wifi_start(settings->wifi_prepare);
  started = true;
  return ESP_OK;
}

static esp_err_t finish_app(pocketjs_system_app_t *app, const uint8_t *bytes, size_t size,
                            pocketjs_system_app_t **out) {
  char error[160];
  app->bytes = bytes;
  app->size = size;
  if (!open_package(bytes, size, &app->package, &app->variant, error, sizeof error)) {
    ESP_LOGE(TAG, "slot %s: %s", app->slot, error);
    pocketjs_system_app_free(app);
    return ESP_ERR_INVALID_RESPONSE;
  }
  *out = app;
  return ESP_OK;
}

esp_err_t pocketjs_system_load_launcher(pocketjs_system_app_t **out_app) {
  if (!started || out_app == NULL) return ESP_ERR_INVALID_STATE;
  pocketjs_system_app_t *app = calloc(1, sizeof *app);
  if (app == NULL) return ESP_ERR_NO_MEM;
  memcpy(app->slot, launcher_slot, sizeof app->slot);
  app->launcher = true;
  return finish_app(app, config.launcher, config.launcher_size, out_app);
}

typedef struct {
  const char *slot;
  uint8_t *bytes;
  size_t size;
} SlotRead;

static void read_slot(void *argument) {
  SlotRead *read = argument;
  char path[SLOT_PATH_BYTES];
  if (launcher_host_slot_path(read->slot, PACKAGE_NAME, path, sizeof path)) read->bytes = read_file(path, &read->size);
}

esp_err_t pocketjs_system_load_slot(const char *slot, pocketjs_system_app_t **out_app) {
  if (!started || out_app == NULL || slot == NULL || strlen(slot) != 16) return ESP_ERR_INVALID_ARG;
  if (strcmp(slot, launcher_slot) == 0) return pocketjs_system_load_launcher(out_app);
  SlotRead read = { .slot = slot };
  system_run(read_slot, &read);
  uint8_t *bytes = read.bytes;
  size_t size = read.size;
  if (bytes == NULL) return ESP_ERR_NOT_FOUND;
  pocketjs_system_app_t *app = calloc(1, sizeof *app);
  if (app == NULL) {
    heap_caps_free(bytes);
    return ESP_ERR_NO_MEM;
  }
  app->owned = bytes;
  snprintf(app->slot, sizeof app->slot, "%s", slot);
  return finish_app(app, bytes, size, out_app);
}

bool pocketjs_system_take_switch(char slot[17], bool *launcher) {
  LauncherTarget target;
  if (!launcher_take_request(&target)) return false;
  memcpy(slot, target.slot, 17);
  *launcher = target.home;
  return true;
}

void pocketjs_system_app_free(pocketjs_system_app_t *app) {
  if (app == NULL) return;
  if (app->package != NULL) pocketjs_package_close(app->package);
  heap_caps_free(app->owned);
  free(app);
}

const pocketjs_package_variant_t *pocketjs_system_app_variant(const pocketjs_system_app_t *app) {
  return app == NULL ? NULL : &app->variant;
}

bool pocketjs_system_app_is_launcher(const pocketjs_system_app_t *app) {
  return app != NULL && app->launcher;
}

static void bind_app(void *argument) {
  const pocketjs_system_app_t *app = argument;
  LauncherPackage facts = facts_of(&app->variant, app->bytes, app->size);
  /* The link names the running app in every net request; the guest's top
   * level may already fetch. */
  launcher_set_current(app->slot, &facts);
  if (plan_feature(&app->variant, "data.fs")) {
    char data[SLOT_PATH_BYTES];
    char temp[SLOT_PATH_BYTES];
    char directory[SLOT_PATH_BYTES];
    if (launcher_host_slot_path(app->slot, NULL, directory, sizeof directory) &&
        launcher_host_slot_path(app->slot, "data", data, sizeof data) &&
        launcher_host_slot_path(app->slot, "fs-tmp", temp, sizeof temp)) {
      mkdir(apps_dir, 0777);
      mkdir(directory, 0777);
      const char *plan = (const char *)app->variant.plan_json.data;
      if (!fs_bind(data, temp, fs_plan_quota(plan, app->variant.plan_json.size))) {
        ESP_LOGW(TAG, "slot %s: data.fs unavailable: %s", app->slot, fs_last_error());
      }
    }
  }
}

esp_err_t pocketjs_system_bind(pocketjs_guest_t *guest, const pocketjs_system_app_t *app) {
  if (!started || guest == NULL || app == NULL) return ESP_ERR_INVALID_ARG;
  system_run(bind_app, (void *)app);
  return system_qjs_install(guest);
}

static void unbind(void *unused) {
  (void)unused;
  link_reset_guest();
  fs_unbind();
}

void pocketjs_system_unbind(void) {
  system_run(unbind, NULL);
}


static void pump(void *unused) {
  (void)unused;
  link_pump();
}

void pocketjs_system_pump(void) {
  system_run(pump, NULL);
}

bool pocketjs_system_switch_pending(void) {
  return launcher_pending();
}

void pocketjs_system_summon(void) {
  launcher_summon();
}

bool pocketjs_system_launcher_running(void) {
  return launcher_home_running();
}

void pocketjs_system_report_failure(const char *message) {
  launcher_report_failure(launcher_current_output(), message);
}
