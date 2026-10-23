//! Motion reuse must be invisible: moving retained pixels and repainting the
//! remaining damage has to produce exactly the bytes of a full render, frame
//! after frame, for content that scrolls, slides past a clip, or is dragged.
//! Offsets are fractional, as eased animations and touch positions are.

use pocketjs_core::raster::render_scaled_rgb565;
use pocketjs_core::{spec, Ui};
use pocketjs_render_rgb565::{
    PpaOps, Rect, RenderTargetState, Renderer, RendererConfig, SrmTransform,
};

const W: u32 = 120;
const H: u32 = 80;
const SCALE: u32 = 2;

/// Declines every PPA operation: the CPU and software paths render.
struct NoPpa;

impl PpaOps for NoPpa {
    fn fill_rgb565(&mut self, _: &mut [u16], _: u32, _: u32, _: Rect, _: u16) -> bool {
        false
    }
    fn blend_a8_rgb565(
        &mut self,
        _: &mut [u16],
        _: u32,
        _: u32,
        _: &[u8],
        _: Rect,
        _: [u8; 3],
        _: u8,
    ) -> bool {
        false
    }
    fn srm_psm5650_to_rgb565(
        &mut self,
        _: &mut [u16],
        _: u32,
        _: u32,
        _: &[u8],
        _: u32,
        _: u32,
        _: Rect,
        _: Rect,
        _: SrmTransform,
    ) -> bool {
        false
    }
}

fn abgr(r: u8, g: u8, b: u8, a: u8) -> f64 {
    (r as u32 | (g as u32) << 8 | (b as u32) << 16 | (a as u32) << 24) as f64
}

fn screen() -> Ui {
    let mut ui = Ui::new_with_raster_density(SCALE);
    ui.set_viewport(W as f32, H as f32);
    ui.set_stable_motion_paint(true);
    let root = spec::ROOT_ID;
    ui.set_prop(root, spec::prop::BG_COLOR, abgr(0, 0, 0, 255));
    ui.set_prop(root, spec::prop::OVERFLOW, spec::Overflow::Hidden as u32 as f64);
    ui
}

fn boxed(ui: &mut Ui, parent: i32, x: f64, y: f64, w: f64, h: f64) -> i32 {
    let node = ui.create_node(0);
    ui.set_prop(node, spec::prop::POS_TYPE, spec::PosType::Absolute as u32 as f64);
    ui.set_prop(node, spec::prop::INSET_L, x);
    ui.set_prop(node, spec::prop::INSET_T, y);
    ui.set_prop(node, spec::prop::WIDTH, w);
    ui.set_prop(node, spec::prop::HEIGHT, h);
    ui.insert_before(parent, node, 0);
    node
}

fn gradient(ui: &mut Ui, node: i32, from: f64, to: f64) {
    ui.set_prop(node, spec::prop::GRAD_FROM, from);
    ui.set_prop(node, spec::prop::GRAD_TO, to);
    ui.set_prop(node, spec::prop::GRAD_DIR, spec::GradDir::ToBottom as u32 as f64);
}

/// Render every frame through motion reuse and compare each with a full
/// render. Returns how many frames moved pixels.
fn play(ui: &mut Ui, frames: usize, mut step: impl FnMut(&mut Ui, usize)) -> usize {
    let (width, height) = (W * SCALE, H * SCALE);
    let mut renderer = Renderer::new(RendererConfig {
        scale: SCALE,
        min_fill_pixels: 1024,
        min_blend_pixels: 256,
        min_srm_pixels: 256,
    })
    .unwrap();
    let mut state = RenderTargetState::new();
    let mut pixels = vec![0u16; (width * height) as usize];
    let mut moved = 0;
    for frame in 0..frames {
        step(ui, frame);
        ui.tick();
        let words = ui.draw().words.clone();
        let plan = renderer.prepare_damage_with_motion(&mut state, &*ui, &words).unwrap();
        moved += plan.motion().is_some() as usize;
        renderer
            .render_incremental_with_motion(&mut state, &*ui, &words, &mut pixels, width, height, &mut NoPpa)
            .unwrap();
        let mut expected = vec![0u16; pixels.len()];
        render_scaled_rgb565(&*ui, &words, &mut expected, SCALE);
        assert!(pixels == expected, "frame {frame} differs from a full render");
    }
    moved
}

#[test]
fn scrolling_gradient_rows_reuse_moved_pixels_exactly() {
    let mut ui = screen();
    let mut rows = Vec::new();
    for i in 0..8 {
        let row = boxed(&mut ui, spec::ROOT_ID, 0.0, i as f64 * 20.0, W as f64, 20.0);
        gradient(&mut ui, row, abgr(20 * i as u8, 60, 220, 255), abgr(10, 30 + 10 * i as u8, 160, 255));
        let label = boxed(&mut ui, row, 6.0, 6.0, 30.0 + 8.0 * i as f64, 8.0);
        ui.set_prop(label, spec::prop::BG_COLOR, abgr(255, 255, 255, 200));
        let line = boxed(&mut ui, row, 0.0, 19.0, W as f64, 1.0);
        ui.set_prop(line, spec::prop::BG_COLOR, abgr(0, 0, 0, 40));
        rows.push(row);
    }
    let moved = play(&mut ui, 8, |ui, frame| {
        for &row in &rows {
            ui.set_prop(row, spec::prop::TRANSLATE_Y, -(frame as f64 * 2.6));
        }
    });
    assert!(moved >= 5, "scrolling frames move pixels ({moved})");
}

#[test]
fn a_rounded_key_panel_sliding_in_from_the_bottom_reuses_pixels_exactly() {
    let mut ui = screen();
    for i in 0..4 {
        let row = boxed(&mut ui, spec::ROOT_ID, 0.0, i as f64 * 20.0, W as f64, 20.0);
        gradient(&mut ui, row, abgr(200, 20, 30, 255), abgr(170, 10, 20, 255));
    }
    let panel = boxed(&mut ui, spec::ROOT_ID, 0.0, 30.0, W as f64, 50.0);
    gradient(&mut ui, panel, abgr(30, 32, 36, 255), abgr(14, 15, 18, 255));
    ui.set_prop(panel, spec::prop::Z_INDEX, 40.0);
    for row in 0..2 {
        for col in 0..5 {
            let key = boxed(&mut ui, panel, 4.0 + col as f64 * 23.0, 4.0 + row as f64 * 23.0, 20.0, 19.0);
            gradient(&mut ui, key, abgr(58, 63, 70, 255), abgr(45, 49, 56, 255));
            ui.set_prop(key, spec::prop::RADIUS, 4.0);
            ui.set_prop(key, spec::prop::SHADOW, 1.0);
            let cap = boxed(&mut ui, key, 7.0, 6.0, 6.0, 7.0);
            ui.set_prop(cap, spec::prop::BG_COLOR, abgr(255, 255, 255, 230));
        }
    }
    let offsets = [52.0, 43.7, 35.2, 26.9, 19.4, 12.3, 6.6, 2.1, 0.4, 0.0];
    let moved = play(&mut ui, offsets.len(), |ui, frame| {
        ui.set_prop(panel, spec::prop::TRANSLATE_Y, offsets[frame]);
    });
    assert!(moved >= 5, "sliding frames move pixels ({moved})");
}

#[test]
fn a_dragged_scaled_row_with_a_shadow_reuses_pixels_exactly() {
    let mut ui = screen();
    for i in 0..4 {
        let row = boxed(&mut ui, spec::ROOT_ID, 0.0, i as f64 * 20.0, W as f64, 20.0);
        gradient(&mut ui, row, abgr(20, 120, 220, 255), abgr(10, 90, 180, 255));
    }
    let lifted = boxed(&mut ui, spec::ROOT_ID, 0.0, 20.0, W as f64, 20.0);
    gradient(&mut ui, lifted, abgr(240, 200, 40, 255), abgr(220, 160, 20, 255));
    ui.set_prop(lifted, spec::prop::Z_INDEX, 30.0);
    ui.set_prop(lifted, spec::prop::SHADOW, 3.0);
    ui.set_prop(lifted, spec::prop::SCALE, 1.05);
    let label = boxed(&mut ui, lifted, 8.0, 6.0, 40.0, 8.0);
    ui.set_prop(label, spec::prop::BG_COLOR, abgr(255, 255, 255, 220));
    let moved = play(&mut ui, 8, |ui, frame| {
        ui.set_prop(lifted, spec::prop::TRANSLATE_Y, frame as f64 * 3.7 + 0.3);
    });
    assert!(moved >= 4, "dragging frames move pixels ({moved})");
}

#[test]
fn a_translucent_layer_never_moves_pixels() {
    let mut ui = screen();
    for i in 0..4 {
        let row = boxed(&mut ui, spec::ROOT_ID, 0.0, i as f64 * 20.0, W as f64, 20.0);
        gradient(&mut ui, row, abgr(20, 120, 220, 255), abgr(10, 90, 180, 255));
    }
    let sheet = boxed(&mut ui, spec::ROOT_ID, 0.0, 10.0, W as f64, 40.0);
    ui.set_prop(sheet, spec::prop::BG_COLOR, abgr(255, 255, 255, 120));
    let moved = play(&mut ui, 6, |ui, frame| {
        ui.set_prop(sheet, spec::prop::TRANSLATE_Y, frame as f64 * 5.0);
    });
    assert_eq!(moved, 0);
}

#[test]
fn rendering_the_same_frame_twice_with_motion_is_a_no_op() {
    let mut ui = screen();
    let row = boxed(&mut ui, spec::ROOT_ID, 0.0, 10.0, W as f64, 30.0);
    gradient(&mut ui, row, abgr(1, 2, 3, 255), abgr(200, 100, 50, 255));
    play(&mut ui, 3, |_, _| {});
}
