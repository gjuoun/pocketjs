import { createHash } from "node:crypto";
import {
  POCKET_IDF_HOST_ABI,
  pocketIdfHostSchema,
  type PocketIdfHostProfile,
} from "../../../contracts/spec/idf-host.ts";
import {
  POCKET_CAPABILITIES,
  definePlatformContractRegistry,
  defineTargetRegistry,
  type PlatformContractRegistry,
} from "../../../contracts/spec/platforms.ts";
import { canonicalJson } from "./plan.ts";
import { createHostExtension, isHostExtension, type HostExtension } from "./host-extension.ts";
import { validateSchema, type ContractDiagnostic, type ValidationResult } from "./validate.ts";

export interface IdfHostBuildPayload {
  readonly profileHash: string;
  readonly tickHz: number;
}

export function readIdfHostExtension(extension: HostExtension | undefined): IdfHostBuildPayload | undefined {
  if (extension === undefined) return undefined;
  if (!isHostExtension(extension)) throw new TypeError("invalid host extension identity");
  if (extension.kind !== "esp-idf") return undefined;
  const { profileHash, tickHz } = extension.payload;
  if (extension.version !== 1 || typeof profileHash !== "string" ||
      !/^sha256:[0-9a-f]{64}$/.test(profileHash) || typeof tickHz !== "number" ||
      !Number.isInteger(tickHz) || tickHz < 1 || tickHz > 240) {
    throw new TypeError("invalid ESP-IDF host extension payload/version");
  }
  return { profileHash, tickHz };
}

export function pocketIdfHostExtension(profileHash: string, tickHz: number): HostExtension {
  const extension = createHostExtension("esp-idf", 1, { profileHash, tickHz });
  readIdfHostExtension(extension);
  return extension;
}

/** Platform environment projection belongs to this adapter, not generic hosts. */
export function idfHostBuildEnvironment(extension: HostExtension): Readonly<Record<string, string>> {
  const payload = readIdfHostExtension(extension);
  if (!payload) throw new TypeError("expected ESP-IDF host extension");
  return { POCKETJS_IDF_PROFILE_HASH: payload.profileHash, POCKETJS_TICK_HZ: String(payload.tickHz) };
}

export function validatePocketIdfHostProfile(input: unknown): ValidationResult<PocketIdfHostProfile> {
  const diagnostics: ContractDiagnostic[] = [];
  validateSchema(input, pocketIdfHostSchema, "", diagnostics);
  if (diagnostics.length === 0) {
    const profile = input as PocketIdfHostProfile;
    const encoded = new TextEncoder().encode(profile.id);
    if (encoded.length >= 16) {
      diagnostics.push({
        code: "idfHost.targetTooLong",
        path: "/id",
        message: "target id must occupy at most 15 UTF-8 bytes",
      });
    }
    const touchSurfaces = [
      ["input.touch", "/display", profile.display.logicalViewports],
      ["input.touch.auxiliary", "/display/auxiliary", profile.display.auxiliary?.logicalViewports ?? []],
    ] as const;
    for (const [capability, path, viewports] of touchSurfaces) {
      if (!profile.capabilities.includes(capability)) continue;
      viewports.forEach((viewport, index) => {
        if (viewport[0] > 1024 || viewport[1] > 1024) {
          diagnostics.push({
            code: "idfHost.touchViewportTooLarge",
            path: `${path}/logicalViewports/${index}`,
            message: "touch-capable logical viewports must fit the 10-bit coordinate contract",
          });
        }
      });
    }
  }
  if (diagnostics.length > 0) return { ok: false, diagnostics };
  return { ok: true, value: input as PocketIdfHostProfile };
}

export function hashPocketIdfHostProfile(profile: PocketIdfHostProfile): string {
  return `sha256:${createHash("sha256").update(canonicalJson(profile)).digest("hex")}`;
}

export function pocketIdfHostRegistry(profile: PocketIdfHostProfile): PlatformContractRegistry {
  return definePlatformContractRegistry(
    POCKET_CAPABILITIES,
    defineTargetRegistry({
      [profile.id]: {
        hostAbi: POCKET_IDF_HOST_ABI,
        platform: profile.platform,
        form: profile.form,
        display: profile.display,
        capabilities: profile.capabilities,
      },
    }),
  );
}
