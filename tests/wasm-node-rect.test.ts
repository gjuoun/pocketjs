import { expect, test } from "bun:test";
import { createWasmUi } from "../hosts/web/wasm-ops.js";
import { PROP, abgr } from "../contracts/spec/spec.ts";

// Root-space border box of a node: untransformed, unclipped, margin excluded.
type NodeRect = { x: number; y: number; w: number; h: number };
const wasmBytes = () => Bun.file(new URL("../hosts/web/pocketjs.wasm", import.meta.url)).arrayBuffer();

test("nodeRect returns the root-space border box of a nested node", async () => {
  const wasm = await createWasmUi(await wasmBytes(), { width: 480, height: 272 });
  const ops = wasm.ops as unknown as { nodeRect?: (id: number) => NodeRect | null };
  const parent = ops.createNode(0);
  ops.setProp(parent, PROP.marginL, 20); ops.setProp(parent, PROP.marginT, 10);
  ops.setProp(parent, PROP.width, 100); ops.setProp(parent, PROP.height, 80);
  ops.setProp(parent, PROP.paddingL, 5); ops.setProp(parent, PROP.paddingT, 3);
  ops.setProp(parent, PROP.bgColor, abgr(0, 0, 255));
  ops.insertBefore(1, parent, 0);
  const child = ops.createNode(0);
  ops.setProp(child, PROP.width, 40); ops.setProp(child, PROP.height, 30);
  ops.setProp(child, PROP.bgColor, abgr(255, 0, 0));
  ops.insertBefore(parent, child, 0);
  wasm.tick();
  expect(ops.nodeRect).toBeFunction();
  expect(ops.nodeRect!(parent)).toEqual({ x: 20, y: 10, w: 100, h: 80 });
  expect(ops.nodeRect!(child)).toEqual({ x: 25, y: 13, w: 40, h: 30 });
});

test("nodeRect is null for the none id and for a destroyed node", async () => {
  const wasm = await createWasmUi(await wasmBytes(), { width: 480, height: 272 });
  const ops = wasm.ops as unknown as { nodeRect?: (id: number) => NodeRect | null };
  const node = ops.createNode(0);
  ops.insertBefore(1, node, 0);
  wasm.tick();
  expect(ops.nodeRect).toBeFunction();
  expect(ops.nodeRect!(0)).toBeNull();
  ops.destroyNode(node);
  expect(ops.nodeRect!(node)).toBeNull();
});
