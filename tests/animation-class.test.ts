// tests/animation-class.test.ts — the animation class a target profile
// derives to, the plan that records it, and animate() on a still display.

import { describe, expect, test } from "bun:test";
import {
  ANIMATION_CLASS_BY_PLATFORM,
  ANIMATION_CLASSES,
  DEFAULT_ANIMATION_CLASS,
  deriveAnimationClass,
} from "../contracts/spec/animation.ts";
import { POCKET_IDF_HOST_SCHEMA_ID, type PocketIdfHostProfile } from "../contracts/spec/idf-host.ts";
import { POCKET_TARGETS, type TargetProfile } from "../contracts/spec/platforms.ts";
import { PROP } from "../contracts/spec/spec.ts";
import { installHost, type HostOps } from "../framework/src/host.ts";
import { hashPocketIdfHostProfile, pocketIdfHostExtension, pocketIdfHostRegistry } from "../framework/src/manifest/idf-host.ts";
import { validateAndResolveBuildPlan } from "../framework/src/manifest/resolve.ts";
import { THREE_DS_DEV_CONTRACTS, THREE_DS_DEV_TARGET_ID } from "../tools/3ds-profile.ts";

const tab5: PocketIdfHostProfile = {
  $schema: POCKET_IDF_HOST_SCHEMA_ID,
  version: 1,
  id: "tab5-test",
  platform: "esp-idf",
  form: "takeover",
  tickHz: 30,
  display: {
    physicalViewport: [480, 272],
    logicalViewports: [[480, 272]],
    presentations: ["native", "integer-fit"],
    rasterDensity: 1,
  },
  capabilities: ["input.analog.left", "input.buttons", "text.glyphs.baked"],
};

const manifest = () => Bun.file(new URL("./fixtures/manifests/portable-psp.json", import.meta.url)).json();

describe("deriveAnimationClass", () => {
  test("every class a platform lists is a class", () => {
    for (const value of Object.values(ANIMATION_CLASS_BY_PLATFORM)) expect(ANIMATION_CLASSES).toContain(value);
    expect(ANIMATION_CLASSES).toContain(DEFAULT_ANIMATION_CLASS);
  });

  test("GPU consoles composite, the ESP-IDF rasterizer repaints, electronic paper is still", () => {
    expect(deriveAnimationClass(POCKET_TARGETS.psp)).toBe("composited");
    expect(deriveAnimationClass(POCKET_TARGETS.vita)).toBe("composited");
    expect(deriveAnimationClass(THREE_DS_DEV_CONTRACTS.targets[THREE_DS_DEV_TARGET_ID])).toBe("composited");
    expect(deriveAnimationClass(POCKET_TARGETS.pocketbook)).toBe("still");
    expect(deriveAnimationClass(pocketIdfHostRegistry(tab5).targets["tab5-test"]!)).toBe("repaint");
  });

  test("a display's own class wins over its platform's", () => {
    const profile: TargetProfile = { ...POCKET_TARGETS.psp, display: { ...POCKET_TARGETS.psp.display, animation: "repaint" } };
    expect(deriveAnimationClass(profile)).toBe("repaint");
  });

  test("a platform the table does not list repaints", () => {
    expect(deriveAnimationClass({ ...POCKET_TARGETS.psp, platform: "calculator" })).toBe(DEFAULT_ANIMATION_CLASS);
  });
});

describe("the plan", () => {
  test("records the class of the target it resolved", async () => {
    const psp = validateAndResolveBuildPlan(await manifest(), { target: "psp" });
    expect(psp.ok).toBe(true);
    if (psp.ok) expect(psp.plan.animation).toBe("composited");
    const idf = validateAndResolveBuildPlan(
      await manifest(),
      { target: tab5.id, hostExtension: pocketIdfHostExtension(hashPocketIdfHostProfile(tab5), tab5.tickHz) },
      pocketIdfHostRegistry(tab5),
    );
    expect(idf.ok).toBe(true);
    if (idf.ok) expect(idf.plan.animation).toBe("repaint");
  });
});

describe("animate() by class", () => {
  const calls: unknown[][] = [];
  const rec = (name: string) => (...args: unknown[]) => void calls.push([name, ...args]);
  const ops = {
    setProp: rec("setProp"),
    animate(...args: number[]) {
      calls.push(["animate", ...args]);
      return 7;
    },
    cancelAnim: rec("cancelAnim"),
  } as unknown as HostOps;

  test("a bundle built without a plan composites, and a tween reaches the core", async () => {
    const { animate, animationClass } = await import("../framework/src/anim.ts");
    expect(animationClass).toBe("composited");
    installHost({ ops, kind: "injected", target: "test", strict: false });
    calls.length = 0;
    expect(animate(5, "opacity", 0, { dur: 100 })).toBe(7);
    expect(calls).toEqual([["animate", 5, PROP.opacity, 0, 100, expect.any(Number), 0]]);
  });

  test("on a still display a tween is its end value and no animation", async () => {
    (globalThis as { __POCKET_ANIMATION__?: string }).__POCKET_ANIMATION__ = "still";
    try {
      const { animate, spring, animationClass } = await import(`../framework/src/anim.ts?still=${Date.now()}`);
      expect(animationClass).toBe("still");
      installHost({ ops, kind: "injected", target: "test", strict: false });
      calls.length = 0;
      expect(animate(5, "translateX", 40)).toBe(0);
      expect(spring(5, "scale", 1)).toBe(0);
      expect(calls).toEqual([
        ["setProp", 5, PROP.translateX, 40],
        ["setProp", 5, PROP.scale, 1],
      ]);
    } finally {
      delete (globalThis as { __POCKET_ANIMATION__?: string }).__POCKET_ANIMATION__;
    }
  });
});
