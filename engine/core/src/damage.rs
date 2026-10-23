//! Backend-independent DrawList damage tracking.
//!
//! A [`DamageTracker`] snapshots the DrawList whose pixels currently live in
//! one persistent render target. On the next frame it compares operations in
//! order, adds both the old and new conservative bounds for changed
//! operations, and returns a small set of disjoint rectangles. Backends must
//! clear each returned rectangle and replay the complete current DrawList
//! clipped to that rectangle so painter order and translucent overlays remain
//! correct.
//!
//! Keep one tracker per physical framebuffer. Core-managed texture, font and
//! style mutations are detected through [`Ui::raster_revision`]. Hosts must
//! still call [`DamageTracker::invalidate`] for output-affecting mutations
//! performed outside `Ui`.
//!
//! [`DamageTracker::prepare_with_motion`] also looks for content that only
//! moved. When operations repeat with every position shifted by one offset
//! and an opaque one among them covers a rectangle that nothing else
//! paints over, the plan carries a [`DamageMotion`]: the backend moves those
//! retained pixels first, then repaints the remaining regions.

use crate::resources::RenderResources;
use alloc::vec::Vec;

use crate::spec;
#[cfg(test)]
use crate::Ui;

const CLIP_DEPTH: usize = 32;

/// Default fixed capacity used by the generic software rasterizer.
pub const DEFAULT_DAMAGE_REGIONS: usize = 8;

/// Failure to plan damage for a persistent target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum DamageError {
    InvalidCapacity,
    InvalidPolicy,
    InvalidTarget,
    MalformedDrawList,
}

/// Integer logical-pixel rectangle with half-open bounds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DamageRect {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
}

impl DamageRect {
    pub const fn new(x0: i32, y0: i32, x1: i32, y1: i32) -> Self {
        Self { x0, y0, x1, y1 }
    }

    pub const fn empty() -> Self {
        Self {
            x0: i32::MAX,
            y0: i32::MAX,
            x1: i32::MIN,
            y1: i32::MIN,
        }
    }

    pub fn intersect(self, other: Self) -> Self {
        Self {
            x0: self.x0.max(other.x0),
            y0: self.y0.max(other.y0),
            x1: self.x1.min(other.x1),
            y1: self.y1.min(other.y1),
        }
    }

    /// `self` minus `hole`, as up to four disjoint rectangles; the empty
    /// ones mark pieces that do not exist.
    pub fn subtract(self, hole: Self) -> [Self; 4] {
        let hole = hole.intersect(self);
        if hole.is_empty() {
            return [self, Self::empty(), Self::empty(), Self::empty()];
        }
        [
            Self::new(self.x0, self.y0, self.x1, hole.y0),
            Self::new(self.x0, hole.y1, self.x1, self.y1),
            Self::new(self.x0, hole.y0, hole.x0, hole.y1),
            Self::new(hole.x1, hole.y0, self.x1, hole.y1),
        ]
    }

    pub fn union(self, other: Self) -> Self {
        if self.is_empty() {
            return other;
        }
        if other.is_empty() {
            return self;
        }
        Self {
            x0: self.x0.min(other.x0),
            y0: self.y0.min(other.y0),
            x1: self.x1.max(other.x1),
            y1: self.y1.max(other.y1),
        }
    }

    pub fn is_empty(self) -> bool {
        self.x0 >= self.x1 || self.y0 >= self.y1
    }

    pub fn area(self) -> u64 {
        if self.is_empty() {
            0
        } else {
            (self.x1 - self.x0) as u64 * (self.y1 - self.y0) as u64
        }
    }

    pub const fn offset(self, dx: i32, dy: i32) -> Self {
        Self {
            x0: self.x0 + dx,
            y0: self.y0 + dy,
            x1: self.x1 + dx,
            y1: self.y1 + dy,
        }
    }

    fn touches(self, other: Self) -> bool {
        !self.is_empty()
            && !other.is_empty()
            && self.x0 <= other.x1
            && other.x0 <= self.x1
            && self.y0 <= other.y1
            && other.y0 <= self.y1
    }
}

impl Default for DamageRect {
    fn default() -> Self {
        Self::empty()
    }
}

/// Backend policy for promoting accumulated damage to a full redraw.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DamagePolicy {
    /// Promote when damaged area reaches this percentage of the viewport.
    /// Valid values are 1 through 100.
    pub full_redraw_percent: u8,
}

impl DamagePolicy {
    pub const fn new(full_redraw_percent: u8) -> Self {
        Self {
            full_redraw_percent,
        }
    }

    fn is_valid(self) -> bool {
        (1..=100).contains(&self.full_redraw_percent)
    }
}

impl Default for DamagePolicy {
    fn default() -> Self {
        Self::new(75)
    }
}

/// Physical target identity used to decide when retained pixels are reusable.
///
/// `signature` is backend-defined and must change when pixel format, clear
/// color, sampling contract, or any other output-affecting configuration
/// changes without changing dimensions.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DamageTarget {
    pub width: u32,
    pub height: u32,
    pub scale: u32,
    pub signature: u64,
}

impl DamageTarget {
    pub const fn new(width: u32, height: u32, scale: u32, signature: u64) -> Self {
        Self {
            width,
            height,
            scale,
            signature,
        }
    }
}

/// Retained pixels a frame reuses by moving them. Every pixel of
/// `destination` equals the previous frame's pixel at `(-dx, -dy)` from it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DamageMotion {
    pub destination: DamageRect,
    pub dx: i32,
    pub dy: i32,
}

impl DamageMotion {
    /// The rectangle of the previous frame whose pixels move.
    pub fn source(&self) -> DamageRect {
        self.destination.offset(-self.dx, -self.dy)
    }
}

/// Damage decision for one frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DamagePlan<const MAX_REGIONS: usize = DEFAULT_DAMAGE_REGIONS> {
    rects: [DamageRect; MAX_REGIONS],
    len: usize,
    full_redraw: bool,
    screen: DamageRect,
    motion: Option<DamageMotion>,
}

impl<const MAX_REGIONS: usize> DamagePlan<MAX_REGIONS> {
    const fn empty(screen: DamageRect) -> Self {
        Self {
            rects: [DamageRect::empty(); MAX_REGIONS],
            len: 0,
            full_redraw: false,
            screen,
            motion: None,
        }
    }

    /// Construct a complete redraw plan for `screen`.
    pub fn full(screen: DamageRect) -> Self {
        let mut damage = Self::empty(screen);
        if MAX_REGIONS > 0 && !screen.is_empty() {
            damage.rects[0] = screen;
            damage.len = 1;
        }
        damage.full_redraw = true;
        damage
    }

    /// Disjoint logical rectangles that must be repainted.
    pub fn regions(&self) -> &[DamageRect] {
        &self.rects[..self.len]
    }

    pub fn region_count(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn area(&self) -> u64 {
        self.regions().iter().map(|rect| rect.area()).sum()
    }

    pub fn bounds(&self) -> DamageRect {
        self.regions()
            .iter()
            .copied()
            .fold(DamageRect::empty(), DamageRect::union)
    }

    pub fn is_full_redraw(&self) -> bool {
        self.full_redraw
    }

    /// Pixels to move before repainting [`regions`](Self::regions). Only
    /// [`DamageTracker::prepare_with_motion`] produces one.
    pub fn motion(&self) -> Option<DamageMotion> {
        self.motion
    }

    /// Apply a backend-selected full-redraw threshold. A plan that moves
    /// pixels keeps them: promoting it would repaint the moved content.
    pub fn with_policy(mut self, policy: DamagePolicy) -> Result<Self, DamageError> {
        if !policy.is_valid() {
            return Err(DamageError::InvalidPolicy);
        }
        if !self.full_redraw && self.motion.is_none() && self.should_promote_to_full(self.screen, policy) {
            self = Self::full(self.screen);
        }
        Ok(self)
    }

    fn should_promote_to_full(&self, screen: DamageRect, policy: DamagePolicy) -> bool {
        let screen_area = screen.area();
        !self.is_empty()
            && screen_area > 0
            && self.area().saturating_mul(100)
                >= screen_area.saturating_mul(policy.full_redraw_percent as u64)
    }

    fn add(&mut self, rect: DamageRect, screen: DamageRect) {
        let mut merged = rect.intersect(screen);
        if merged.is_empty() {
            return;
        }

        let mut index = 0usize;
        while index < self.len {
            if merged.touches(self.rects[index]) {
                merged = merged.union(self.rects[index]);
                self.remove(index);
                index = 0;
            } else {
                index += 1;
            }
        }

        if self.len < MAX_REGIONS {
            self.rects[self.len] = merged;
            self.len += 1;
            return;
        }

        let mut best = 0usize;
        let mut best_inflation = u64::MAX;
        for (candidate, &existing) in self.regions().iter().enumerate() {
            let union = existing.union(merged);
            let inflation = union
                .area()
                .saturating_sub(existing.area())
                .saturating_sub(merged.area());
            if inflation < best_inflation {
                best = candidate;
                best_inflation = inflation;
            }
        }
        merged = merged.union(self.rects[best]);
        self.remove(best);
        self.add(merged, screen);
    }

    fn remove(&mut self, index: usize) {
        self.len -= 1;
        self.rects[index] = self.rects[self.len];
        self.rects[self.len] = DamageRect::empty();
    }
}

/// DrawList snapshot associated with one persistent render target.
pub struct DamageTracker<const MAX_REGIONS: usize = DEFAULT_DAMAGE_REGIONS> {
    words: Vec<u32>,
    target: DamageTarget,
    raster_revision: u64,
    valid: bool,
    /// Decoded operations of `words`, when `ops_valid`.
    ops: Vec<MotionOp>,
    ops_valid: bool,
    /// The list [`prepare_with_motion`](Self::prepare_with_motion) decoded
    /// last, adopted by a [`commit`](Self::commit) of the same words.
    pending_words: Vec<u32>,
    pending_ops: Vec<MotionOp>,
    pending: bool,
    /// Working storage of the motion planner, kept between frames.
    scratch: MotionScratch,
}

/// Buffers the motion planner refills every frame.
#[derive(Default)]
struct MotionScratch {
    pairs: Vec<OpPair>,
    covers: Vec<(usize, DamageRect)>,
    obstacles: Vec<(usize, DamageRect)>,
}

impl<const MAX_REGIONS: usize> DamageTracker<MAX_REGIONS> {
    pub const fn new() -> Self {
        Self {
            words: Vec::new(),
            target: DamageTarget::new(0, 0, 0, 0),
            raster_revision: 0,
            valid: false,
            ops: Vec::new(),
            ops_valid: false,
            pending_words: Vec::new(),
            pending_ops: Vec::new(),
            pending: false,
            scratch: MotionScratch { pairs: Vec::new(), covers: Vec::new(), obstacles: Vec::new() },
        }
    }

    /// Force the next frame to repaint the complete target.
    pub fn invalidate(&mut self) {
        self.valid = false;
        self.pending = false;
    }

    /// Compute damage without modifying the stored snapshot.
    ///
    /// Call [`commit`](Self::commit) only after the returned regions have
    /// successfully been rendered.
    pub fn prepare(
        &self,
        ui: &impl RenderResources,
        words: &[u32],
        target: DamageTarget,
    ) -> Result<DamagePlan<MAX_REGIONS>, DamageError> {
        if MAX_REGIONS == 0 {
            return Err(DamageError::InvalidCapacity);
        }
        if target.scale == 0 {
            return Err(DamageError::InvalidTarget);
        }
        let screen = target_screen(ui, target)?;
        let full_redraw =
            !self.valid || self.target != target || self.raster_revision != ui.raster_revision();
        let damage = if full_redraw {
            validate_draw_list(ui, words, screen)?;
            DamagePlan::full(screen)
        } else {
            draw_list_damage(ui, &self.words, words, screen)?
        };
        Ok(damage)
    }

    /// [`prepare`](Self::prepare) that may also return a [`DamageMotion`].
    /// The caller must move the motion's pixels before repainting regions.
    /// The snapshot is unchanged; the decoded list is kept for the
    /// [`commit`](Self::commit) that records it.
    pub fn prepare_with_motion(
        &mut self,
        ui: &impl RenderResources,
        words: &[u32],
        target: DamageTarget,
    ) -> Result<DamagePlan<MAX_REGIONS>, DamageError> {
        if MAX_REGIONS == 0 {
            return Err(DamageError::InvalidCapacity);
        }
        if target.scale == 0 {
            return Err(DamageError::InvalidTarget);
        }
        let screen = target_screen(ui, target)?;
        let full = !self.valid || self.target != target || self.raster_revision != ui.raster_revision();
        if !full && same_words(&self.words, words) {
            return Ok(DamagePlan::empty(screen));
        }
        // The pending buffers hold nothing worth keeping until this returns.
        self.pending = false;
        decode_ops_into(ui, words, screen, &mut self.pending_ops)?;
        let plan = if full {
            DamagePlan::full(screen)
        } else {
            if !self.ops_valid {
                decode_ops_into(ui, &self.words, screen, &mut self.ops)?;
                self.ops_valid = true;
            }
            motion_damage(&self.words, &self.ops, words, &self.pending_ops, screen, &mut self.scratch)
        };
        self.pending_words.clear();
        self.pending_words.extend_from_slice(words);
        self.pending = true;
        Ok(plan)
    }

    /// Record the DrawList after its [`DamagePlan`] has been rendered.
    pub fn commit(&mut self, ui: &impl RenderResources, words: &[u32], target: DamageTarget) {
        if !same_words(&self.words, words) {
            if self.pending && same_words(&self.pending_words, words) {
                self.adopt_pending();
            } else {
                self.words.clear();
                self.words.extend_from_slice(words);
                self.ops_valid = false;
            }
        }
        self.finish_commit(ui, target);
    }

    /// [`commit`](Self::commit) the list the last
    /// [`prepare_with_motion`](Self::prepare_with_motion) decoded, for a
    /// caller that rendered exactly that list: the words copied at prepare
    /// time are kept without comparing them again. Returns false, changing
    /// nothing, when no prepared list is pending.
    pub fn commit_prepared(&mut self, ui: &impl RenderResources, target: DamageTarget) -> bool {
        if !self.pending {
            return false;
        }
        self.adopt_pending();
        self.finish_commit(ui, target);
        true
    }

    fn adopt_pending(&mut self) {
        core::mem::swap(&mut self.words, &mut self.pending_words);
        core::mem::swap(&mut self.ops, &mut self.pending_ops);
        self.ops_valid = true;
    }

    fn finish_commit(&mut self, ui: &impl RenderResources, target: DamageTarget) {
        self.pending = false;
        self.target = target;
        self.raster_revision = ui.raster_revision();
        self.valid = true;
    }
}

impl<const MAX_REGIONS: usize> Default for DamageTracker<MAX_REGIONS> {
    fn default() -> Self {
        Self::new()
    }
}

fn target_screen(
    ui: &impl RenderResources,
    target: DamageTarget,
) -> Result<DamageRect, DamageError> {
    let (viewport_width, viewport_height) = ui.viewport();
    if viewport_width <= 0.0 || viewport_height <= 0.0 {
        return Err(DamageError::InvalidTarget);
    }
    let logical_width = viewport_width as u32;
    let logical_height = viewport_height as u32;
    if logical_width
        .checked_mul(target.scale)
        .ok_or(DamageError::InvalidTarget)?
        != target.width
        || logical_height
            .checked_mul(target.scale)
            .ok_or(DamageError::InvalidTarget)?
            != target.height
    {
        return Err(DamageError::InvalidTarget);
    }
    Ok(DamageRect::new(
        0,
        0,
        logical_width as i32,
        logical_height as i32,
    ))
}

struct DecodedOp<'a> {
    code: u32,
    words: &'a [u32],
    bounds: DamageRect,
}

struct DamageDecoder<'a> {
    words: &'a [u32],
    index: usize,
    screen: DamageRect,
    clip: DamageRect,
    stack: [DamageRect; CLIP_DEPTH],
    depth: usize,
}

impl<'a> DamageDecoder<'a> {
    fn new(words: &'a [u32], screen: DamageRect) -> Self {
        Self {
            words,
            index: 0,
            screen,
            clip: screen,
            stack: [screen; CLIP_DEPTH],
            depth: 0,
        }
    }

    fn next(&mut self, ui: &impl RenderResources) -> Result<Option<DecodedOp<'a>>, ()> {
        if self.index == self.words.len() {
            return Ok(None);
        }
        let start = self.index;
        let code = *self.words.get(start).ok_or(())?;
        let len = match code {
            spec::draw_op::RECT => 4,
            spec::draw_op::GRAD_RECT => 6,
            spec::draw_op::GLYPH_RUN => {
                let count = (self.words.get(start + 1).copied().ok_or(())? >> 16) as usize;
                3usize
                    .checked_add(count.checked_mul(2).ok_or(())?)
                    .ok_or(())?
            }
            spec::draw_op::TEX_QUAD => 9,
            spec::draw_op::SCISSOR => 3,
            spec::draw_op::SCISSOR_POP => 1,
            spec::draw_op::TRI => 7,
            spec::draw_op::TEX_TRI => 12,
            spec::draw_op::TEXT_RUN => {
                // 8 header words + ceil(byteLen/4) packed UTF-8 words.
                let bytes = *self.words.get(start + 7).ok_or(())? as usize;
                8usize.checked_add(bytes.div_ceil(4)).ok_or(())?
            }
            spec::draw_op::SURFACE_QUAD => 9,
            _ => return Err(()),
        };
        let end = start.checked_add(len).ok_or(())?;
        let words = self.words.get(start..end).ok_or(())?;
        self.index = end;

        let bounds = match code {
            spec::draw_op::RECT | spec::draw_op::GRAD_RECT => {
                logical_rect(words[1], words[2]).intersect(self.clip)
            }
            spec::draw_op::GLYPH_RUN => glyph_run_bounds(ui, words, self.clip),
            spec::draw_op::TEX_QUAD => logical_rect(words[2], words[3]).intersect(self.clip),
            spec::draw_op::SCISSOR => {
                if self.depth >= self.stack.len() {
                    return Err(());
                }
                self.stack[self.depth] = self.clip;
                self.depth += 1;
                self.clip = self.screen.intersect(logical_rect(words[1], words[2]));
                self.clip
            }
            spec::draw_op::SCISSOR_POP => {
                if self.depth == 0 {
                    return Err(());
                }
                self.depth -= 1;
                self.clip = self.stack[self.depth];
                DamageRect::empty()
            }
            spec::draw_op::TRI => triangle_bounds([words[1], words[2], words[3]], self.clip),
            spec::draw_op::TEX_TRI => triangle_bounds([words[2], words[5], words[8]], self.clip),
            // Native-text runs carry no glyph geometry the tracker can
            // measure; the core keeps every partially-clipped run inside a
            // scissor, so the current clip is a sound (conservative) bound.
            spec::draw_op::TEXT_RUN => self.clip,
            spec::draw_op::SURFACE_QUAD => logical_rect(words[6], words[7]).intersect(self.clip),
            _ => return Err(()),
        };
        Ok(Some(DecodedOp {
            code,
            words,
            bounds,
        }))
    }

    fn is_balanced(&self) -> bool {
        self.depth == 0
    }
}

fn draw_list_damage<const MAX_REGIONS: usize>(
    ui: &impl RenderResources,
    previous: &[u32],
    current: &[u32],
    screen: DamageRect,
) -> Result<DamagePlan<MAX_REGIONS>, DamageError> {
    if same_words(previous, current) {
        return Ok(DamagePlan::empty(screen));
    }

    let mut old = DamageDecoder::new(previous, screen);
    let mut new = DamageDecoder::new(current, screen);
    let mut damage = DamagePlan::empty(screen);
    loop {
        let old_op = old.next(ui).map_err(|_| DamageError::MalformedDrawList)?;
        let new_op = new.next(ui).map_err(|_| DamageError::MalformedDrawList)?;
        match (old_op, new_op) {
            (None, None) => break,
            (Some(old_op), Some(new_op)) if old_op.code == new_op.code => {
                if !same_words(old_op.words, new_op.words) {
                    damage.add(old_op.bounds, screen);
                    damage.add(new_op.bounds, screen);
                }
            }
            _ => return Ok(DamagePlan::full(screen)),
        }
    }
    if !old.is_balanced() || !new.is_balanced() {
        return Err(DamageError::MalformedDrawList);
    }
    Ok(damage)
}

/// Word-by-word equality. Comparing eight words at a time with XOR keeps
/// short operations inline instead of a `memcmp` call per operation.
#[inline]
fn same_words(a: &[u32], b: &[u32]) -> bool {
    a.len() == b.len()
        && a.chunks(8)
            .zip(b.chunks(8))
            .all(|(x, y)| x.iter().zip(y).fold(0, |diff, (p, q)| diff | (p ^ q)) == 0)
}

/// One decoded operation of a DrawList: its word range and clipped bounds.
#[derive(Clone, Copy)]
struct MotionOp {
    code: u32,
    start: usize,
    end: usize,
    bounds: DamageRect,
}

/// How an operation of the current list relates to the previous one.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Relation {
    Same,
    /// Identical words except every position shifted by (dx, dy).
    Moved(i16, i16),
    Changed,
}

/// Operation index meaning "absent from this list" in an [`OpPair`].
const NO_OP: u32 = u32::MAX;

/// One step of the alignment: an operation of each list, or of one list
/// when it appeared or disappeared.
#[derive(Clone, Copy)]
struct OpPair {
    old: u32,
    new: u32,
    relation: Relation,
}

impl OpPair {
    fn new(old: Option<usize>, new: Option<usize>, relation: Relation) -> Self {
        let index = |op: Option<usize>| op.map_or(NO_OP, |op| op as u32);
        Self { old: index(old), new: index(new), relation }
    }

    fn old(&self) -> Option<usize> {
        (self.old != NO_OP).then_some(self.old as usize)
    }

    fn new_op(&self) -> Option<usize> {
        (self.new != NO_OP).then_some(self.new as usize)
    }
}

/// Operations searched ahead for a match after an insertion or removal.
const RESYNC_WINDOW: usize = 128;
/// Smallest destination worth a move, in logical pixels.
const MIN_MOTION_AREA: u64 = 1024;
/// Opaque covers evaluated per candidate offset, largest first.
const MAX_COVERS: usize = 16;

/// Decode `words` into `ops`, reusing its allocation.
fn decode_ops_into(
    ui: &impl RenderResources,
    words: &[u32],
    screen: DamageRect,
    ops: &mut Vec<MotionOp>,
) -> Result<(), DamageError> {
    let mut decoder = DamageDecoder::new(words, screen);
    ops.clear();
    loop {
        let start = decoder.index;
        let Some(op) = decoder.next(ui).map_err(|_| DamageError::MalformedDrawList)? else {
            break;
        };
        ops.push(MotionOp {
            code: op.code,
            start,
            end: decoder.index,
            bounds: op.bounds,
        });
    }
    if !decoder.is_balanced() {
        return Err(DamageError::MalformedDrawList);
    }
    Ok(())
}

fn shift(old: u32, new: u32) -> (i32, i32) {
    let (ox, oy) = xy(old);
    let (nx, ny) = xy(new);
    (nx - ox, ny - oy)
}

/// The common offset of `new` from `old`, zero when the words are equal,
/// when the words differ only by positions shifted together. One pass over
/// the words.
fn translation(code: u32, old: &[u32], new: &[u32]) -> Option<(i32, i32)> {
    if old.len() != new.len() {
        return None;
    }
    match code {
        spec::draw_op::RECT | spec::draw_op::GRAD_RECT => {
            same_words(&old[2..], &new[2..]).then(|| shift(old[1], new[1]))
        }
        spec::draw_op::TEX_QUAD => {
            (old[1] == new[1] && same_words(&old[3..], &new[3..])).then(|| shift(old[2], new[2]))
        }
        spec::draw_op::SCISSOR => (old[2] == new[2]).then(|| shift(old[1], new[1])),
        spec::draw_op::TRI => {
            let d = shift(old[1], new[1]);
            (same_words(&old[4..], &new[4..]) && shift(old[2], new[2]) == d && shift(old[3], new[3]) == d)
                .then_some(d)
        }
        spec::draw_op::GLYPH_RUN if old.len() >= 5 => {
            if !same_words(&old[1..3], &new[1..3]) {
                return None;
            }
            let d = shift(old[3], new[3]);
            let glyphs = old[3..].chunks_exact(2).zip(new[3..].chunks_exact(2));
            for (a, b) in glyphs {
                if a[1] != b[1] || shift(a[0], b[0]) != d {
                    return None;
                }
            }
            Some(d)
        }
        _ => same_words(old, new).then_some((0, 0)),
    }
}

fn relate(previous: &[u32], old: &MotionOp, current: &[u32], new: &MotionOp) -> Relation {
    let a = &previous[old.start..old.end];
    let b = &current[new.start..new.end];
    match translation(old.code, a, b) {
        Some((0, 0)) => Relation::Same,
        Some((dx, dy)) => match (i16::try_from(dx), i16::try_from(dy)) {
            (Ok(dx), Ok(dy)) => Relation::Moved(dx, dy),
            _ => Relation::Changed,
        },
        None => Relation::Changed,
    }
}

/// Whether a pair's relation continues the latest motion: unchanged, or
/// moved by the same offset (any offset before the first move).
fn continues(relation: Relation, offset: Option<(i16, i16)>) -> bool {
    match relation {
        Relation::Same => true,
        Relation::Moved(dx, dy) => offset.is_none_or(|offset| offset == (dx, dy)),
        Relation::Changed => false,
    }
}

/// Pair operations in painter order, resynchronizing across operations that
/// appear or disappear (content entering or leaving a clip). Repeated
/// content (a row of identical keys) also matches its neighbor moved by one
/// item, so a pair that breaks the latest offset first looks for a resync
/// that keeps it.
fn align_into(previous: &[u32], old: &[MotionOp], current: &[u32], new: &[MotionOp], pairs: &mut Vec<OpPair>) {
    pairs.clear();
    let (mut i, mut j) = (0usize, 0usize);
    let mut offset = None;
    while i < old.len() || j < new.len() {
        if i == old.len() {
            pairs.push(OpPair::new(None, Some(j), Relation::Changed));
            j += 1;
            continue;
        }
        if j == new.len() {
            pairs.push(OpPair::new(Some(i), None, Relation::Changed));
            i += 1;
            continue;
        }
        let same_code = old[i].code == new[j].code;
        let relation = if same_code {
            relate(previous, &old[i], current, &new[j])
        } else {
            Relation::Changed
        };
        if continues(relation, offset) {
            if let Relation::Moved(dx, dy) = relation {
                offset = Some((dx, dy));
            }
            pairs.push(OpPair::new(Some(i), Some(j), relation));
            i += 1;
            j += 1;
            continue;
        }
        // Same-code edits look one operation ahead; code mismatches and
        // moves by another offset search the whole window.
        let window = if same_code && relation == Relation::Changed { 1 } else { RESYNC_WINDOW };
        let strong = |i: usize, j: usize| {
            old[i].code == new[j].code && continues(relate(previous, &old[i], current, &new[j]), offset)
        };
        // The nearest resync wins; an insertion wins a tie.
        let resync = (1..=window).find_map(|k| {
            if j + k < new.len() && strong(i, j + k) {
                Some((0, k))
            } else if i + k < old.len() && strong(i + k, j) {
                Some((k, 0))
            } else {
                None
            }
        });
        match resync {
            Some((0, k)) => {
                for t in j..j + k {
                    pairs.push(OpPair::new(None, Some(t), Relation::Changed));
                }
                j += k;
            }
            Some((k, _)) => {
                for t in i..i + k {
                    pairs.push(OpPair::new(Some(t), None, Relation::Changed));
                }
                i += k;
            }
            None => {
                // A move by another offset starts a new motion.
                if let Relation::Moved(dx, dy) = relation {
                    offset = Some((dx, dy));
                }
                pairs.push(OpPair::new(Some(i), Some(j), relation));
                i += 1;
                j += 1;
            }
        }
    }
}

/// The largest part of `rect` outside `obstacle`.
fn cut(rect: DamageRect, obstacle: DamageRect) -> DamageRect {
    if rect.intersect(obstacle).is_empty() {
        return rect;
    }
    [
        DamageRect::new(rect.x0, rect.y0, rect.x1, obstacle.y0.min(rect.y1)),
        DamageRect::new(rect.x0, obstacle.y1.max(rect.y0), rect.x1, rect.y1),
        DamageRect::new(rect.x0, rect.y0, obstacle.x0.min(rect.x1), rect.y1),
        DamageRect::new(obstacle.x1.max(rect.x0), rect.y0, rect.x1, rect.y1),
    ]
    .into_iter()
    .max_by_key(|piece| piece.area())
    .unwrap_or(DamageRect::empty())
}


/// RECT or GRAD_RECT that paints every pixel of its bounds opaquely.
fn opaque_cover(words: &[u32], op: &MotionOp) -> bool {
    let w = &words[op.start..op.end];
    match op.code {
        spec::draw_op::RECT => w[3] >> 24 == 255,
        spec::draw_op::GRAD_RECT => w[3] >> 24 == 255 && w[4] >> 24 == 255,
        _ => false,
    }
}

/// Scissors paint nothing; a clip change shows up as different clipped
/// bounds on the operations it affects.
fn painted(op: &MotionOp) -> bool {
    op.code != spec::draw_op::SCISSOR && op.code != spec::draw_op::SCISSOR_POP
}

/// The largest rectangle whose pixels all moved by `(dx, dy)`: covered by an
/// opaque operation that follows the offset, and painted over only by
/// operations that follow it. An operation follows the offset where both
/// frames paint it when it moved by the offset, or when it is a flat RECT
/// of one color, whose pixels do not depend on position.
#[allow(clippy::too_many_arguments)]
fn safe_rect(
    previous: &[u32],
    current: &[u32],
    old: &[MotionOp],
    new: &[MotionOp],
    pairs: &[OpPair],
    (dx, dy): (i16, i16),
    covers: &mut Vec<(usize, DamageRect)>,
    obstacles: &mut Vec<(usize, DamageRect)>,
) -> DamageRect {
    let moved = Relation::Moved(dx, dy);
    let (dx, dy) = (dx as i32, dy as i32);
    let flat = |o: usize, n: usize| {
        old[o].code == spec::draw_op::RECT
            && new[n].code == spec::draw_op::RECT
            && previous[old[o].start + 3] == current[new[n].start + 3]
    };
    covers.clear();
    // Rectangles whose pixels may differ from the moved ones, in painter
    // order of the pair that paints them.
    obstacles.clear();
    for (index, pair) in pairs.iter().enumerate() {
        match (pair.old(), pair.new_op()) {
            (Some(o), Some(n)) if pair.relation == moved || flat(o, n) => {
                if !painted(&new[n]) {
                    continue;
                }
                let before = old[o].bounds.offset(dx, dy);
                let both = new[n].bounds.intersect(before);
                if opaque_cover(current, &new[n]) && !both.is_empty() {
                    covers.push((index, both));
                }
                if new[n].bounds != before {
                    let pieces = new[n].bounds.subtract(before).into_iter().chain(before.subtract(new[n].bounds));
                    obstacles.extend(pieces.filter(|piece| !piece.is_empty()).map(|piece| (index, piece)));
                }
            }
            (o, n) => {
                if let Some(n) = n.filter(|&n| painted(&new[n]) && !new[n].bounds.is_empty()) {
                    obstacles.push((index, new[n].bounds));
                }
                if let Some(o) = o.filter(|&o| painted(&old[o]) && !old[o].bounds.is_empty()) {
                    obstacles.push((index, old[o].bounds.offset(dx, dy)));
                }
            }
        }
    }
    covers.sort_by_key(|(_, cover)| core::cmp::Reverse(cover.area()));
    covers.truncate(MAX_COVERS);
    let mut safe: Vec<DamageRect> = Vec::with_capacity(covers.len());
    for &(index, mut cover) in covers.iter() {
        let later = obstacles.partition_point(|&(at, _)| at <= index);
        for &(_, obstacle) in &obstacles[later..] {
            if cover.is_empty() {
                break;
            }
            cover = cut(cover, obstacle);
        }
        if !cover.is_empty() {
            safe.push(cover);
        }
    }
    // Safety holds per pixel, so rectangles that share a full edge merge.
    let mut best = DamageRect::empty();
    while let Some(start) = (0..safe.len()).max_by_key(|&k| safe[k].area()) {
        let mut merged = safe.swap_remove(start);
        loop {
            let next = safe.iter().position(|r| {
                (r.x0 == merged.x0 && r.x1 == merged.x1 && r.y0 <= merged.y1 && merged.y0 <= r.y1)
                    || (r.y0 == merged.y0 && r.y1 == merged.y1 && r.x0 <= merged.x1 && merged.x0 <= r.x1)
            });
            let Some(next) = next else { break };
            merged = merged.union(safe.swap_remove(next));
        }
        if merged.area() > best.area() {
            best = merged;
        }
    }
    best
}

fn motion_damage<const MAX_REGIONS: usize>(
    previous: &[u32],
    old: &[MotionOp],
    current: &[u32],
    new: &[MotionOp],
    screen: DamageRect,
    scratch: &mut MotionScratch,
) -> DamagePlan<MAX_REGIONS> {
    let MotionScratch { pairs, covers, obstacles } = scratch;
    align_into(previous, old, current, new, pairs);
    let mut damage = DamagePlan::empty(screen);
    let mut offsets: Vec<((i16, i16), u64)> = Vec::new();
    for pair in pairs.iter() {
        if pair.relation == Relation::Same {
            if let (Some(o), Some(n)) = (pair.old(), pair.new_op()) {
                if painted(&new[n]) && old[o].bounds != new[n].bounds {
                    damage.add(old[o].bounds, screen);
                    damage.add(new[n].bounds, screen);
                }
            }
            continue;
        }
        if let Some(o) = pair.old().filter(|&o| painted(&old[o])) {
            damage.add(old[o].bounds, screen);
        }
        if let Some(n) = pair.new_op().filter(|&n| painted(&new[n])) {
            damage.add(new[n].bounds, screen);
            if let Relation::Moved(dx, dy) = pair.relation {
                let area = new[n].bounds.area();
                match offsets.iter_mut().find(|(d, _)| *d == (dx, dy)) {
                    Some((_, total)) => *total += area,
                    None => offsets.push(((dx, dy), area)),
                }
            }
        }
    }
    offsets.sort_by_key(|&(_, area)| core::cmp::Reverse(area));
    // Only damaged pixels need to move; the rest already hold their value.
    let useful = |safe: DamageRect| {
        damage
            .regions()
            .iter()
            .fold(DamageRect::empty(), |bounds, &region| bounds.union(region.intersect(safe)))
    };
    let motion = offsets
        .iter()
        .take(3)
        .filter(|&&(_, area)| area >= MIN_MOTION_AREA)
        .map(|&(d, _)| (d, useful(safe_rect(previous, current, old, new, pairs, d, covers, obstacles).intersect(screen))))
        .max_by_key(|(_, rect)| rect.area())
        .filter(|(_, rect)| rect.area() >= MIN_MOTION_AREA);
    let Some(((dx, dy), destination)) = motion else {
        return damage;
    };
    let mut remaining = DamagePlan::empty(screen);
    for &region in damage.regions() {
        for piece in region.subtract(destination) {
            remaining.add(piece, screen);
        }
    }
    remaining.motion = Some(DamageMotion { destination, dx: dx as i32, dy: dy as i32 });
    remaining
}

fn validate_draw_list(
    ui: &impl RenderResources,
    words: &[u32],
    screen: DamageRect,
) -> Result<(), DamageError> {
    let mut decoder = DamageDecoder::new(words, screen);
    while decoder
        .next(ui)
        .map_err(|_| DamageError::MalformedDrawList)?
        .is_some()
    {}
    if decoder.is_balanced() {
        Ok(())
    } else {
        Err(DamageError::MalformedDrawList)
    }
}

fn glyph_run_bounds(ui: &impl RenderResources, words: &[u32], clip: DamageRect) -> DamageRect {
    if words.len() < 3 || words[2] >> 24 == 0 {
        return DamageRect::empty();
    }
    let slot = (words[1] & 0xff) as u8;
    let Some(atlas) = ui.font_atlas(slot) else {
        return DamageRect::empty();
    };
    let mut bounds = DamageRect::empty();
    for glyph in words[3..].chunks_exact(2) {
        let gid = (glyph[1] & 0xffff) as u16;
        if gid >= atlas.glyph_count {
            continue;
        }
        let (x, y) = xy(glyph[0]);
        bounds = bounds.union(DamageRect::new(
            x,
            y,
            x + atlas.cell_w as i32,
            y + atlas.cell_h as i32,
        ));
    }
    bounds.intersect(clip)
}

fn triangle_bounds(vertices: [u32; 3], clip: DamageRect) -> DamageRect {
    let [(x0, y0), (x1, y1), (x2, y2)] = vertices.map(xy);
    DamageRect::new(
        x0.min(x1).min(x2),
        y0.min(y1).min(y2),
        x0.max(x1).max(x2),
        y0.max(y1).max(y2),
    )
    .intersect(clip)
}

#[inline]
fn xy(word: u32) -> (i32, i32) {
    (
        (word & 0xffff) as u16 as i16 as i32,
        (word >> 16) as u16 as i16 as i32,
    )
}

#[inline]
fn wh(word: u32) -> (i32, i32) {
    ((word & 0xffff) as i32, (word >> 16) as i32)
}

#[inline]
fn logical_rect(xy_word: u32, wh_word: u32) -> DamageRect {
    let (x, y) = xy(xy_word);
    let (w, h) = wh(wh_word);
    DamageRect::new(x, y, x + w, y + h)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn xy_word(x: i16, y: i16) -> u32 {
        x as u16 as u32 | ((y as u16 as u32) << 16)
    }

    fn wh_word(w: u16, h: u16) -> u32 {
        w as u32 | ((h as u32) << 16)
    }

    fn target(width: u32, height: u32, scale: u32) -> DamageTarget {
        DamageTarget::new(width, height, scale, 1)
    }

    fn frame(left: u32, right: u32) -> Vec<u32> {
        vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(32, 16),
            0xff20_1008,
            spec::draw_op::RECT,
            xy_word(2, 4),
            wh_word(4, 4),
            left,
            spec::draw_op::RECT,
            xy_word(26, 4),
            wh_word(4, 4),
            right,
        ]
    }

    #[test]
    fn first_unchanged_and_disjoint_frames_are_classified() {
        let mut ui = Ui::new();
        ui.set_viewport(32.0, 16.0);
        let previous = frame(0xff00_00ff, 0xff00_ff00);
        let current = frame(0xffff_0000, 0xffff_ffff);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();

        let first = tracker.prepare(&ui, &previous, target(32, 16, 1)).unwrap();
        assert!(first.is_full_redraw());
        assert_eq!(first.area(), 32 * 16);
        tracker.commit(&ui, &previous, target(32, 16, 1));

        let unchanged = tracker.prepare(&ui, &previous, target(32, 16, 1)).unwrap();
        assert!(!unchanged.is_full_redraw());
        assert!(unchanged.is_empty());

        let changed = tracker.prepare(&ui, &current, target(32, 16, 1)).unwrap();
        assert!(!changed.is_full_redraw());
        assert_eq!(changed.region_count(), 2);
        assert_eq!(changed.area(), 32);
    }

    #[test]
    fn structure_target_and_invalidation_force_full_redraws() {
        let mut ui = Ui::new();
        ui.set_viewport(16.0, 8.0);
        let previous = vec![
            spec::draw_op::RECT,
            xy_word(1, 1),
            wh_word(3, 3),
            0xff00_00ff,
        ];
        let current = Vec::new();
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &previous, target(16, 8, 1));

        let structural = tracker.prepare(&ui, &current, target(16, 8, 1)).unwrap();
        assert!(structural.is_full_redraw());

        tracker.commit(&ui, &current, target(16, 8, 1));
        tracker.invalidate();
        assert!(tracker
            .prepare(&ui, &current, target(16, 8, 1))
            .unwrap()
            .is_full_redraw());

        tracker.commit(&ui, &current, target(16, 8, 1));
        assert!(tracker
            .prepare(&ui, &current, target(32, 16, 2))
            .unwrap()
            .is_full_redraw());

        tracker.commit(&ui, &current, target(32, 16, 2));
        assert!(tracker
            .prepare(&ui, &current, DamageTarget::new(32, 16, 2, 2),)
            .unwrap()
            .is_full_redraw());
    }

    #[test]
    fn policy_promotes_large_damage_and_rejects_invalid_configuration() {
        let mut ui = Ui::new();
        ui.set_viewport(10.0, 10.0);
        let previous = vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(8, 10),
            0xff00_00ff,
        ];
        let current = vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(8, 10),
            0xff00_ff00,
        ];
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &previous, target(10, 10, 1));
        let plan = tracker
            .prepare(&ui, &current, target(10, 10, 1))
            .unwrap()
            .with_policy(DamagePolicy::new(75))
            .unwrap();
        assert!(plan.is_full_redraw());

        assert_eq!(
            tracker
                .prepare(&ui, &current, target(10, 10, 1))
                .unwrap()
                .with_policy(DamagePolicy::new(0)),
            Err(DamageError::InvalidPolicy)
        );
        let empty_tracker = DamageTracker::<0>::new();
        assert_eq!(
            empty_tracker.prepare(&ui, &current, target(10, 10, 1)),
            Err(DamageError::InvalidCapacity)
        );
    }

    #[test]
    fn malformed_drawlists_are_rejected_and_prepare_does_not_commit() {
        let mut ui = Ui::new();
        ui.set_viewport(16.0, 8.0);
        let previous = vec![
            spec::draw_op::RECT,
            xy_word(1, 1),
            wh_word(3, 3),
            0xff00_00ff,
        ];
        let current = vec![
            spec::draw_op::RECT,
            xy_word(4, 1),
            wh_word(3, 3),
            0xff00_ff00,
        ];
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &previous, target(16, 8, 1));

        let first_plan = tracker.prepare(&ui, &current, target(16, 8, 1)).unwrap();
        let second_plan = tracker.prepare(&ui, &current, target(16, 8, 1)).unwrap();
        assert_eq!(first_plan, second_plan);

        let fresh = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        assert_eq!(
            fresh.prepare(&ui, &[spec::draw_op::RECT, xy_word(0, 0)], target(16, 8, 1),),
            Err(DamageError::MalformedDrawList)
        );
        assert_eq!(
            fresh.prepare(
                &ui,
                &[spec::draw_op::SCISSOR, xy_word(0, 0), wh_word(4, 4),],
                target(16, 8, 1),
            ),
            Err(DamageError::MalformedDrawList)
        );
    }

    #[test]
    fn capacity_merges_regions_and_fractional_viewports_match_raster_semantics() {
        let mut ui = Ui::new();
        ui.set_viewport(30.0, 10.0);
        let frame = |colors: [u32; 3]| {
            let mut words = Vec::new();
            for (index, color) in colors.into_iter().enumerate() {
                words.extend_from_slice(&[
                    spec::draw_op::RECT,
                    xy_word((index * 10 + 1) as i16, 2),
                    wh_word(2, 2),
                    color,
                ]);
            }
            words
        };
        let previous = frame([0xff00_00ff, 0xff00_ff00, 0xffff_0000]);
        let current = frame([0xffff_ffff, 0xff80_8080, 0xff00_0000]);
        let mut tracker = DamageTracker::<2>::new();
        tracker.commit(&ui, &previous, target(30, 10, 1));
        let plan = tracker.prepare(&ui, &current, target(30, 10, 1)).unwrap();
        assert_eq!(plan.region_count(), 2);
        assert!(plan.area() >= 12);

        ui.set_viewport(7.5, 5.5);
        let fractional = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new()
            .prepare(&ui, &[], target(7, 5, 1))
            .unwrap();
        assert!(fractional.is_full_redraw());
    }

    #[test]
    fn default_capacity_merges_a_ninth_disjoint_region() {
        assert_eq!(DEFAULT_DAMAGE_REGIONS, 8);
        let mut ui = Ui::new();
        ui.set_viewport(96.0, 8.0);
        let frame = |color: u32| {
            let mut words = Vec::new();
            for index in 0..9 {
                words.extend_from_slice(&[
                    spec::draw_op::RECT,
                    xy_word((index * 10 + 1) as i16, 2),
                    wh_word(2, 2),
                    color,
                ]);
            }
            words
        };
        let previous = frame(0xff00_00ff);
        let current = frame(0xff00_ff00);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &previous, target(96, 8, 1));

        let plan = tracker.prepare(&ui, &current, target(96, 8, 1)).unwrap();
        assert!(!plan.is_full_redraw());
        assert_eq!(plan.region_count(), DEFAULT_DAMAGE_REGIONS);
        assert!(plan.area() > 9 * 4);
    }

    #[test]
    fn core_resource_revision_invalidates_unchanged_drawlists() {
        let mut ui = Ui::new();
        ui.set_viewport(4.0, 2.0);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &[], target(4, 2, 1));
        assert!(tracker
            .prepare(&ui, &[], target(4, 2, 1))
            .unwrap()
            .is_empty());

        assert!(ui.upload_texture(&[0xff, 0xff, 0xff, 0xff], 1, 1, spec::psm::PSM_8888,) >= 0);
        assert!(tracker
            .prepare(&ui, &[], target(4, 2, 1))
            .unwrap()
            .is_full_redraw());
    }

    // ---- motion ----------------------------------------------------------

    const W: i32 = 64;
    const H: i32 = 48;

    /// A RECT or GRAD_RECT clipped to the screen the way the core emits it.
    fn clipped(code: u32, x: i32, y: i32, w: i32, h: i32, colors: &[u32]) -> Vec<u32> {
        let (x0, y0, x1, y1) = (x.max(0), y.max(0), (x + w).min(W), (y + h).min(H));
        if x0 >= x1 || y0 >= y1 {
            return Vec::new();
        }
        let mut op = vec![code, xy_word(x0 as i16, y0 as i16), wh_word((x1 - x0) as u16, (y1 - y0) as u16)];
        op.extend_from_slice(colors);
        op
    }

    fn rect(x: i32, y: i32, w: i32, h: i32, color: u32) -> Vec<u32> {
        clipped(spec::draw_op::RECT, x, y, w, h, &[color])
    }

    /// A gradient as a clip-invariant DrawList emits it: partially clipped
    /// ones stay unclipped inside a screen scissor.
    fn grad(x: i32, y: i32, w: i32, h: i32, from: u32, to: u32) -> Vec<u32> {
        let visible = clipped(spec::draw_op::GRAD_RECT, x, y, w, h, &[from, to, 1]);
        if visible.is_empty() || (x >= 0 && y >= 0 && x + w <= W && y + h <= H) {
            return visible;
        }
        vec![
            spec::draw_op::SCISSOR,
            xy_word(0, 0),
            wh_word(W as u16, H as u16),
            spec::draw_op::GRAD_RECT,
            xy_word(x as i16, y as i16),
            wh_word(w as u16, h as u16),
            from,
            to,
            spec::GradDir::ToBottom as u32,
            spec::draw_op::SCISSOR_POP,
        ]
    }

    /// A panel: opaque background, translucent keys, and a 1px edge.
    fn panel(y: i32) -> Vec<u32> {
        let mut words = grad(0, y, W, 30, 0xff40_2010, 0xff10_0804);
        for (i, kx) in [2, 18, 34, 50].into_iter().enumerate() {
            words.extend(rect(kx, y + 4 + (i as i32 % 2) * 12, 12, 10, 0x80ff_ffff));
        }
        words.extend(rect(0, y, W, 1, 0x40ff_ffff));
        words
    }

    fn rows(scroll: i32) -> Vec<u32> {
        let mut words = rect(0, 0, W, H, 0xff00_0000);
        for row in 0..10 {
            let y = row * 8 - scroll;
            words.extend(grad(0, y, W, 8, 0xff00_40f0 + row as u32, 0xff00_20c0 + row as u32));
            words.extend(rect(4, y + 2, 20 + row * 3, 4, 0xc0ff_ffff));
        }
        words
    }

    /// Render `previous`, plan `current` with motion, apply it, and require
    /// the result to equal a full render of `current`.
    fn motion_plan(ui: &Ui, previous: &[u32], current: &[u32], scale: u32) -> DamagePlan {
        let (width, height) = (W as usize * scale as usize, H as usize * scale as usize);
        let target = DamageTarget::new(width as u32, height as u32, scale, 1);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        let mut pixels = vec![0u16; width * height];
        crate::raster::render_scaled_rgb565(ui, previous, &mut pixels, scale);
        tracker.commit(ui, previous, target);
        let plan = tracker.prepare_with_motion(ui, current, target).unwrap();
        if let Some(motion) = plan.motion() {
            let before = pixels.clone();
            let s = scale as i32;
            let destination = motion.destination;
            for y in destination.y0 * s..destination.y1 * s {
                for x in destination.x0 * s..destination.x1 * s {
                    let from = (y - motion.dy * s) as usize * width + (x - motion.dx * s) as usize;
                    pixels[y as usize * width + x as usize] = before[from];
                }
            }
        }
        crate::raster::render_scaled_rgb565_regions(ui, current, &mut pixels, scale, plan.regions());
        let mut expected = vec![0u16; width * height];
        crate::raster::render_scaled_rgb565(ui, current, &mut expected, scale);
        assert_eq!(pixels, expected, "motion plus damage must equal a full render");
        plan
    }

    fn screen_ui() -> Ui {
        let mut ui = Ui::new();
        ui.set_viewport(W as f32, H as f32);
        ui
    }

    #[test]
    fn a_panel_sliding_past_the_screen_edge_moves_its_visible_pixels() {
        let ui = screen_ui();
        let mut previous = rows(0);
        previous.extend(panel(30));
        let mut current = rows(0);
        current.extend(panel(24));
        for scale in [1, 2] {
            let plan = motion_plan(&ui, &previous, &current, scale);
            let motion = plan.motion().expect("the panel moved");
            assert_eq!((motion.dx, motion.dy), (0, -6));
            // Rows the panel brings in at the bottom edge stay damage; keys
            // the edge clips keep their pixels where both frames show them.
            assert_eq!(motion.destination, DamageRect::new(0, 24, W, H - 6));
            assert!(plan.area() < (W * H) as u64 / 2);
        }
    }

    #[test]
    fn scrolled_rows_merge_into_one_moved_rectangle() {
        let ui = screen_ui();
        let plan = motion_plan(&ui, &rows(0), &rows(3), 2);
        let motion = plan.motion().expect("the rows moved");
        assert_eq!((motion.dx, motion.dy), (0, -3));
        assert_eq!((motion.destination.x0, motion.destination.x1), (0, W));
        assert!(motion.destination.area() >= (W * 32) as u64);
    }

    #[test]
    fn a_fixed_overlay_cuts_the_moved_rectangle() {
        let ui = screen_ui();
        let overlay = rect(4, 38, 12, 6, 0x80ff_0000);
        let mut previous = rows(0);
        previous.extend(panel(20));
        previous.extend(&overlay);
        let mut current = rows(0);
        current.extend(panel(16));
        current.extend(&overlay);
        let plan = motion_plan(&ui, &previous, &current, 1);
        let motion = plan.motion().expect("the panel moved");
        assert!(motion.destination.intersect(DamageRect::new(4, 38, 16, 44)).is_empty());
    }

    #[test]
    fn translucent_content_does_not_move_pixels() {
        let ui = screen_ui();
        let sheet = |y: i32| {
            let mut words = rows(0);
            words.extend(rect(0, y, W, 20, 0x80ff_ffff));
            words
        };
        let plan = motion_plan(&ui, &sheet(10), &sheet(6), 1);
        assert!(plan.motion().is_none());
    }

    #[test]
    fn two_movers_keep_exact_pixels() {
        let ui = screen_ui();
        let scene = |a: i32, b: i32| {
            let mut words = rect(0, 0, W, H, 0xff10_1010);
            words.extend(grad(0, a, 32, 36, 0xff80_0000, 0xff40_0000));
            words.extend(rect(2, a + 2, 10, 4, 0x90ff_ffff));
            words.extend(grad(32, b, 32, 36, 0xff00_8000, 0xff00_4000));
            words.extend(rect(36, b + 2, 10, 4, 0x90ff_ffff));
            words
        };
        let plan = motion_plan(&ui, &scene(4, 10), &scene(9, 6), 2);
        assert!(plan.motion().is_some());
        motion_plan(&ui, &scene(9, 14), &scene(9, 14), 2);
    }

    #[test]
    fn a_growing_scissor_repaints_what_it_reveals() {
        let ui = screen_ui();
        let scene = |h: u16| {
            let mut words = rect(0, 0, W, H, 0xff00_0000);
            words.extend([spec::draw_op::SCISSOR, xy_word(0, 0), wh_word(W as u16, h)]);
            words.extend(rect(0, 0, W, H, 0xff80_4020));
            words.extend(rect(10, 10, 20, 30, 0x80ff_ffff));
            words.push(spec::draw_op::SCISSOR_POP);
            words
        };
        let plan = motion_plan(&ui, &scene(20), &scene(32), 1);
        assert_eq!(plan.bounds(), DamageRect::new(0, 0, W, 32));
    }

    #[test]
    fn a_static_translucent_bar_keeps_the_rows_under_it_moving() {
        let ui = screen_ui();
        let bar = rect(0, 10, W, 20, 0x6000_0000);
        let mut previous = rows(0);
        previous.extend(&bar);
        let mut current = rows(3);
        current.extend(&bar);
        let plan = motion_plan(&ui, &previous, &current, 2);
        let motion = plan.motion().expect("the rows moved");
        assert_eq!((motion.dx, motion.dy), (0, -3));
        // A flat color paints the same pixels wherever both frames show the
        // bar, so only its edges stay damage.
        assert!(motion.destination.y0 <= 10 && motion.destination.y1 >= 27);
    }

    #[test]
    fn a_motion_plan_is_not_promoted_to_a_full_redraw() {
        let ui = screen_ui();
        let target = DamageTarget::new(W as u32, H as u32, 1, 1);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &rows(0), target);
        let plan = tracker.prepare_with_motion(&ui, &rows(5), target).unwrap();
        assert!(plan.motion().is_some());
        let promoted = plan.with_policy(DamagePolicy::new(1)).unwrap();
        assert!(!promoted.is_full_redraw());
        assert_eq!(promoted.motion(), plan.motion());
    }

    #[test]
    fn committing_the_prepared_list_plans_the_next_frame_like_a_fresh_tracker() {
        let ui = screen_ui();
        let target = DamageTarget::new(W as u32, H as u32, 1, 1);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        assert!(!tracker.commit_prepared(&ui, target), "nothing was prepared");
        tracker.commit(&ui, &rows(0), target);
        tracker.prepare_with_motion(&ui, &rows(2), target).unwrap();
        assert!(tracker.commit_prepared(&ui, target));
        let next = tracker.prepare_with_motion(&ui, &rows(5), target).unwrap();
        let mut fresh = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        fresh.commit(&ui, &rows(2), target);
        let expected = fresh.prepare_with_motion(&ui, &rows(5), target).unwrap();
        assert_eq!(next.motion(), expected.motion());
        assert_eq!(next.regions(), expected.regions());
    }

    #[test]
    fn subtracting_a_hole_leaves_disjoint_pieces() {
        let rect = DamageRect::new(0, 0, 10, 10);
        let pieces = rect.subtract(DamageRect::new(2, 3, 6, 7));
        let area: u64 = pieces.iter().filter(|p| !p.is_empty()).map(|p| p.area()).sum();
        assert_eq!(area, 100 - 16);
        assert_eq!(rect.subtract(DamageRect::new(20, 20, 30, 30))[0], rect);
    }

    #[test]
    fn plain_prepare_never_moves_pixels() {
        let ui = screen_ui();
        let target = DamageTarget::new(W as u32, H as u32, 1, 1);
        let mut tracker = DamageTracker::<DEFAULT_DAMAGE_REGIONS>::new();
        tracker.commit(&ui, &rows(0), target);
        assert!(tracker.prepare(&ui, &rows(3), target).unwrap().motion().is_none());
    }
}
