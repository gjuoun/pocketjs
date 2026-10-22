import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { resolveMotoGPlayBuildPlan } from "../tools/moto-g-play-profile.ts";
import { extractHostBuildInputs } from "../framework/src/manifest/host-build-inputs.ts";
test("Moto Clear resolves full-panel logical geometry and requires the implemented offload capability", () => {
  const manifest = JSON.parse(readFileSync("apps/clear/pocket.android.json", "utf8"));
  const plan = resolveMotoGPlayBuildPlan(manifest);
  const inputs = extractHostBuildInputs(plan, { expectedTarget: "moto-g-play-dev" });
  expect(inputs.viewport).toEqual({ logical: [360, 800], physical: [720, 1600], rasterDensity: 2, presentation: "native" });
  manifest.app.viewport.fixed.logical = [320, 480];
  expect(() => resolveMotoGPlayBuildPlan(manifest)).toThrow();
});

test("the Android CLI rejects removed or unknown profiles before invoking tools", () => {
  for (const profile of ["blackberry-android", "unknown"]) {
    const result = Bun.spawnSync([
      process.execPath, "tools/android.ts", `--profile=${profile}`, "doctor",
    ], { stdout: "pipe", stderr: "pipe" });
    expect(result.exitCode).not.toBe(0);
    expect(result.stderr.toString()).toContain(`Unsupported Android profile: ${profile}`);
    expect(result.stdout.toString()).toBe("");
  }
});

test("the Android CLI refuses --guest outside build-app", () => {
  const result = Bun.spawnSync([
    process.execPath, "tools/android.ts", "--profile=moto-g-play", "doctor", "--guest=guest-dir",
  ], { stdout: "pipe", stderr: "pipe" });
  expect(result.exitCode).not.toBe(0);
  expect(result.stderr.toString()).toContain("--guest goes with build-app");
});

test("the Android CLI refuses --guest without a plan for the identity and viewport", () => {
  const result = Bun.spawnSync([
    process.execPath, "tools/android.ts", "--profile=moto-g-play", "build-app", "--guest=guest-dir",
  ], { stdout: "pipe", stderr: "pipe" });
  expect(result.exitCode).not.toBe(0);
  expect(result.stderr.toString()).toContain("--guest needs --plan");
});
