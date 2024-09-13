/* See LICENSE for license details. */
#include "beamformer.h"

static Color
colour_from_normalized(v4 rgba)
{
	return (Color){.r = rgba.r * 255.0f, .g = rgba.g * 255.0f,
	               .b = rgba.b * 255.0f, .a = rgba.a * 255.0f};
}

static Color
fade(Color a, f32 alpha)
{
	a.a = (u8)((f32)a.a * alpha);
	return a;
}

static f32
move_towards_f32(f32 current, f32 target, f32 delta)
{
	if (target < current) {
		current -= delta;
		if (current < target)
			current = target;
	} else {
		current += delta;
		if (current > target)
			current = target;
	}
	return current;
}

static f32
lerp(f32 a, f32 b, f32 t)
{
	return a + t * (b - a);
}

static v4
lerp_v4(v4 a, v4 b, f32 t)
{
	return (v4){
		.x = a.x + t * (b.x - a.x),
		.y = a.y + t * (b.y - a.y),
		.z = a.z + t * (b.z - a.z),
		.w = a.w + t * (b.w - a.w),
	};
}

static v2
measure_text(Font font, s8 text)
{
	v2 result = {.y = font.baseSize};
	for (size i = 0; i < text.len; i++) {
		/* NOTE: assumes font glyphs are ordered ASCII */
		i32 idx   = (i32)text.data[i] - 0x20;
		result.x += font.glyphs[idx].advanceX;
		if (font.glyphs[idx].advanceX == 0)
			result.x += (font.recs[idx].width + font.glyphs[idx].offsetX);
	}
	return result;
}

static v2
draw_text(Font font, s8 text, v2 pos, f32 rotation, Color colour)
{
	rlPushMatrix();

	rlTranslatef(pos.x, pos.y, 0);
	rlRotatef(rotation, 0, 0, 1);

	v2 off = {0};
	for (size i = 0; i < text.len; i++) {
		/* NOTE: assumes font glyphs are ordered ASCII */
		i32 idx = text.data[i] - 0x20;
		Rectangle dst = {
			off.x + font.glyphs[idx].offsetX - font.glyphPadding,
			off.y + font.glyphs[idx].offsetY - font.glyphPadding,
			font.recs[idx].width  + 2.0f * font.glyphPadding,
			font.recs[idx].height + 2.0f * font.glyphPadding
		};
		Rectangle src = {
			font.recs[idx].x - font.glyphPadding,
			font.recs[idx].y - font.glyphPadding,
			font.recs[idx].width  + 2.0f * font.glyphPadding,
			font.recs[idx].height + 2.0f * font.glyphPadding
		};
		DrawTexturePro(font.texture, src, dst, (Vector2){0}, 0, colour);

		off.x += font.glyphs[idx].advanceX;
		if (font.glyphs[idx].advanceX == 0)
			off.x += font.recs[idx].width;
	}
	rlPopMatrix();
	v2 result = {.x = off.x, .y = font.baseSize};
	return result;
}

static Rect
scale_rect_centered(Rect r, v2 scale)
{
	Rect or   = r;
	r.size.w *= scale.x;
	r.size.h *= scale.y;
	r.pos.x  += (or.size.w - r.size.w) / 2;
	r.pos.y  += (or.size.h - r.size.h) / 2;
	return r;
}

static v2
center_align_text_in_rect(Rect r, s8 text, Font font)
{
	v2 ts    = measure_text(font, text);
	v2 delta = { .w = r.size.w - ts.w, .h = r.size.h - ts.h };
	return (v2) {
		.x = r.pos.x + 0.5 * delta.w,
	        .y = r.pos.y + 0.5 * delta.h,
	};
}

static b32
bmv_equal(BPModifiableValue *a, BPModifiableValue *b)
{
	b32 result = (uintptr_t)a->value == (uintptr_t)b->value;
	return result;
}

static f32
bmv_scaled_value(BPModifiableValue *a)
{
	f32 result;
	if (a->flags & MV_FLOAT) result = *(f32 *)a->value * a->scale;
	else                     result = *(i32 *)a->value * a->scale;
	return result;
}

static void
bmv_store_value(BeamformerCtx *ctx, BPModifiableValue *bmv, f32 new_val, b32 from_scroll)
{
	if (bmv->flags & MV_FLOAT) {
		f32 *value = bmv->value;
		if (new_val / bmv->scale == *value)
			return;
		*value = new_val / bmv->scale;
		CLAMP(*value, bmv->flimits.x, bmv->flimits.y);
	} else if (bmv->flags & MV_INT && bmv->flags & MV_POWER_OF_TWO) {
		i32 *value = bmv->value;
		if (new_val == *value)
			return;
		if (from_scroll && new_val > *value) *value <<= 1;
		else                                 *value = round_down_power_of_2(new_val);
		CLAMP(*value, bmv->ilimits.x, bmv->ilimits.y);
	} else {
		ASSERT(bmv->flags & MV_INT);
		i32 *value = bmv->value;
		if (new_val / bmv->scale == *value)
			return;
		*value = new_val / bmv->scale;
		CLAMP(*value, bmv->ilimits.x, bmv->ilimits.y);
	}
	if (bmv->flags & MV_CAUSES_COMPUTE) {
		ctx->flags |= DO_COMPUTE;
		ctx->params->upload = 1;
	}
	if (bmv->flags & MV_GEN_MIPMAPS)
		ctx->flags |= GEN_MIPMAPS;
}

static s8
bmv_sprint(BPModifiableValue *bmv, s8 buf)
{
	s8 result = buf;
	if (bmv->flags & MV_FLOAT) {
		f32 *value = bmv->value;
		size len = snprintf((char *)buf.data, buf.len, "%0.02f", *value * bmv->scale);
		ASSERT(len <= buf.len);
		result.len = len;
	} else {
		ASSERT(bmv->flags & MV_INT);
		i32 *value = bmv->value;
		size len = snprintf((char *)buf.data, buf.len, "%d", (i32)(*value * bmv->scale));
		ASSERT(len <= buf.len);
		result.len = len;
	}
	return result;
}

static void
do_text_input(BeamformerCtx *ctx, i32 max_disp_chars, Rect r, Color colour)
{
	v2 ts  = measure_text(ctx->font, (s8){.len = ctx->is.buf_len, .data = (u8 *)ctx->is.buf});
	v2 pos = {.x = r.pos.x, .y = r.pos.y + (r.size.y - ts.y) / 2};

	i32 buf_delta = ctx->is.buf_len - max_disp_chars;
	if (buf_delta < 0) buf_delta = 0;
	s8 buf = {.len = ctx->is.buf_len - buf_delta, .data = (u8 *)ctx->is.buf + buf_delta};
	{
		/* NOTE: drop a char if the subtext still doesn't fit */
		v2 nts = measure_text(ctx->font, buf);
		if (nts.w > 0.96 * r.size.w) {
			buf.data++;
			buf.len--;
		}
	}
	draw_text(ctx->font, buf, pos, 0, colour);

	ctx->is.cursor_blink_t = move_towards_f32(ctx->is.cursor_blink_t,
	                                          ctx->is.cursor_blink_target, 1.5 * ctx->dt);
	if (ctx->is.cursor_blink_t == ctx->is.cursor_blink_target) {
		if (ctx->is.cursor_blink_target == 0) ctx->is.cursor_blink_target = 1;
		else                                  ctx->is.cursor_blink_target = 0;
	}

	v4 bg = FOCUSED_COLOUR;
	bg.a  = 0;
	Color cursor_colour = colour_from_normalized(lerp_v4(bg, FOCUSED_COLOUR,
	                                                     ctx->is.cursor_blink_t));


	/* NOTE: guess a cursor position */
	if (ctx->is.cursor == -1) {
		/* NOTE: extra offset to help with putting a cursor at idx 0 */
		#define TEXT_HALF_CHAR_WIDTH 10
		f32 x_off = TEXT_HALF_CHAR_WIDTH, x_bounds = r.size.w * ctx->is.cursor_hover_p;
		i32 i;
		for (i = 0; i < ctx->is.buf_len && x_off < x_bounds; i++) {
			/* NOTE: assumes font glyphs are ordered ASCII */
			i32 idx  = ctx->is.buf[i] - 0x20;
			x_off   += ctx->font.glyphs[idx].advanceX;
			if (ctx->font.glyphs[idx].advanceX == 0)
				x_off += ctx->font.recs[idx].width;
		}
		ctx->is.cursor = i;
	}

	buf.len = ctx->is.cursor - buf_delta;
	v2 sts = measure_text(ctx->font, buf);
	f32 cursor_x = r.pos.x + sts.x;
	f32 cursor_width;
	if (ctx->is.cursor == ctx->is.buf_len) cursor_width = MIN(ctx->window_size.w * 0.03, 20);
	else                                   cursor_width = MIN(ctx->window_size.w * 0.01, 6);

	Rect cursor_r = {
		.pos  = {.x = cursor_x,     .y = pos.y},
		.size = {.w = cursor_width, .h = ts.h},
	};

	DrawRectanglePro(cursor_r.rl, (Vector2){0}, 0, cursor_colour);

	/* NOTE: handle multiple input keys on a single frame */
	i32 key = GetCharPressed();
	while (key > 0) {
		if (ctx->is.buf_len == (ARRAY_COUNT(ctx->is.buf) - 1)) {
			ctx->is.buf[ARRAY_COUNT(ctx->is.buf) - 1] = 0;
			break;
		}

		b32 allow_key = ((key >= '0' && key <= '9') || (key == '.') ||
		                 (key == '-' && ctx->is.cursor == 0));
		if (allow_key) {
			mem_move(ctx->is.buf + ctx->is.cursor,
			         ctx->is.buf + ctx->is.cursor + 1,
			         ctx->is.buf_len - ctx->is.cursor + 1);

			ctx->is.buf[ctx->is.cursor++] = key;
			ctx->is.buf_len++;
		}
		key = GetCharPressed();
	}

	if ((IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) && ctx->is.cursor > 0)
		ctx->is.cursor--;

	if ((IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) &&
	    ctx->is.cursor < ctx->is.buf_len)
		ctx->is.cursor++;

	if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) &&
	    ctx->is.cursor > 0) {
		ctx->is.cursor--;
		mem_move(ctx->is.buf + ctx->is.cursor + 1,
		         ctx->is.buf + ctx->is.cursor,
		         ctx->is.buf_len - ctx->is.cursor);
		ctx->is.buf_len--;
	}
	if ((IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) &&
	    ctx->is.cursor < ctx->is.buf_len) {
		mem_move(ctx->is.buf + ctx->is.cursor + 1,
		         ctx->is.buf + ctx->is.cursor,
		         ctx->is.buf_len - ctx->is.cursor);
		ctx->is.buf_len--;
	}
}

static void
set_text_input_idx(BeamformerCtx *ctx, BPModifiableValue bmv, Rect r, v2 mouse)
{
	if (ctx->is.store.value && !bmv_equal(&ctx->is.store, &bmv)) {
		f32 new_val = strtof(ctx->is.buf, NULL);
		bmv_store_value(ctx, &ctx->is.store, new_val, 0);
	}

	ctx->is.store  = bmv;
	ctx->is.cursor = -1;

	if (ctx->is.store.value == NULL)
		return;

	s8 ibuf = bmv_sprint(&bmv, (s8){.data = (u8 *)ctx->is.buf, .len = ARRAY_COUNT(ctx->is.buf)});
	ctx->is.buf_len = ibuf.len;

	ASSERT(CheckCollisionPointRec(mouse.rl, r.rl));
	ctx->is.cursor_hover_p = (mouse.x - r.pos.x) / r.size.w;
	CLAMP01(ctx->is.cursor_hover_p);
}

/* NOTE: This is kinda sucks no matter how you spin it. If we want the values to be
 * left aligned in the center column we need to know the longest prefix length but
 * without either hardcoding one of the prefixes as the longest one or measuring all
 * of them we can't know this ahead of time. For now we hardcode this and manually
 * adjust when needed */
#define LISTING_LEFT_COLUMN_WIDTH 270.0f
#define LISTING_LINE_PAD           6.0f

static Rect
do_value_listing(s8 prefix, s8 suffix, f32 value, Font font, Arena a, Rect r)
{
	v2 suffix_s = measure_text(font, suffix);
	v2 suffix_p = {.x = r.pos.x + r.size.w - suffix_s.w, .y = r.pos.y};

	s8 txt   = s8alloc(&a, 64);
	txt.len  = snprintf((char *)txt.data, txt.len, "%0.02f", value);
	v2 txt_p = {.x = r.pos.x + LISTING_LEFT_COLUMN_WIDTH, .y = r.pos.y};

	draw_text(font, prefix, r.pos,    0, colour_from_normalized(FG_COLOUR));
	draw_text(font, txt,    txt_p,    0, colour_from_normalized(FG_COLOUR));
	draw_text(font, suffix, suffix_p, 0, colour_from_normalized(FG_COLOUR));
	r.pos.y  += suffix_s.h + LISTING_LINE_PAD;
	r.size.y -= suffix_s.h + LISTING_LINE_PAD;

	return r;
}

static Rect
do_text_input_listing(s8 prefix, s8 suffix, BPModifiableValue bmv, BeamformerCtx *ctx, Arena a,
                      Rect r, v2 mouse, f32 *hover_t)
{
	s8 buf = s8alloc(&a, 64);
	s8 txt = buf;
	v2 txt_s;

	b32 bmv_active = bmv_equal(&bmv, &ctx->is.store);
	if (bmv_active) {
		txt_s = measure_text(ctx->font, (s8){.len = ctx->is.buf_len,
		                                     .data = (u8 *)ctx->is.buf});
	} else {
		txt   = bmv_sprint(&bmv, buf);
		txt_s = measure_text(ctx->font, txt);
	}

	Rect edit_rect = {
		.pos  = {.x = r.pos.x + LISTING_LEFT_COLUMN_WIDTH, .y = r.pos.y},
		.size = {.x = txt_s.w + TEXT_BOX_EXTRA_X, .y = txt_s.h}
	};

	b32 collides = CheckCollisionPointRec(mouse.rl, edit_rect.rl);
	if (collides && !bmv_active) *hover_t += TEXT_HOVER_SPEED * ctx->dt;
	else                         *hover_t -= TEXT_HOVER_SPEED * ctx->dt;
	CLAMP01(*hover_t);

	if (collides) {
		f32 mouse_scroll = GetMouseWheelMove();
		if (mouse_scroll) {
			if (bmv_active)
				set_text_input_idx(ctx, (BPModifiableValue){0}, (Rect){0}, mouse);
			f32 old_val = bmv_scaled_value(&bmv);
			bmv_store_value(ctx, &bmv, old_val + mouse_scroll, 1);
			txt = bmv_sprint(&bmv, buf);
		}
	}

	if (!collides && bmv_equal(&bmv, &ctx->is.store) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		set_text_input_idx(ctx, (BPModifiableValue){0}, (Rect){0}, mouse);
		txt = bmv_sprint(&bmv, buf);
	}

	if (collides && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
		set_text_input_idx(ctx, bmv, edit_rect, mouse);

	Color colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t));

	if (!bmv_equal(&bmv, &ctx->is.store)) {
		draw_text(ctx->font, txt, edit_rect.pos, 0, colour);
	} else {
		do_text_input(ctx, 7, edit_rect, colour);
	}

	v2 suffix_s = measure_text(ctx->font, suffix);
	v2 suffix_p = {.x = r.pos.x + r.size.w - suffix_s.w, .y = r.pos.y};
	draw_text(ctx->font, prefix, r.pos,    0, colour_from_normalized(FG_COLOUR));
	draw_text(ctx->font, suffix, suffix_p, 0, colour_from_normalized(FG_COLOUR));

	r.pos.y  += suffix_s.h + LISTING_LINE_PAD;
	r.size.y -= suffix_s.h + LISTING_LINE_PAD;

	return r;
}

static Rect
do_text_toggle_listing(s8 prefix, s8 text0, s8 text1, b32 toggle, BPModifiableValue bmv,
                       BeamformerCtx *ctx, Rect r, v2 mouse, f32 *hover_t)
{
	v2 txt_s;
	if (toggle) txt_s = measure_text(ctx->font, text1);
	else        txt_s = measure_text(ctx->font, text0);

	Rect edit_rect = {
		.pos  = {.x = r.pos.x + LISTING_LEFT_COLUMN_WIDTH, .y = r.pos.y},
		.size = {.x = txt_s.w + TEXT_BOX_EXTRA_X, .y = txt_s.h}
	};

	b32 collides = CheckCollisionPointRec(mouse.rl, edit_rect.rl);
	if (collides) *hover_t += TEXT_HOVER_SPEED * ctx->dt;
	else          *hover_t -= TEXT_HOVER_SPEED * ctx->dt;
	CLAMP01(*hover_t);

	b32 pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
	if (collides && (pressed || GetMouseWheelMove())) {
		toggle = !toggle;
		bmv_store_value(ctx, &bmv, toggle, 0);
	}

	Color colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t));
	draw_text(ctx->font, prefix, r.pos, 0, colour_from_normalized(FG_COLOUR));
	draw_text(ctx->font, toggle? text1: text0, edit_rect.pos, 0, colour);

	r.pos.y  += txt_s.h + LISTING_LINE_PAD;
	r.size.y -= txt_s.h + LISTING_LINE_PAD;

	return r;
}

static b32
do_text_button(BeamformerCtx *ctx, s8 text, Rect r, v2 mouse, f32 *hover_t)
{
	b32 hovered  = CheckCollisionPointRec(mouse.rl, r.rl);
	b32 pressed  = 0;
	pressed     |= (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
	pressed     |= (hovered && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT));

	if (hovered) *hover_t += TEXT_HOVER_SPEED * ctx->dt;
	else         *hover_t -= TEXT_HOVER_SPEED * ctx->dt;
	CLAMP01(*hover_t);

	f32 param  = lerp(1, 1.04, *hover_t);
	v2  bscale = (v2){
		.x = param + RECT_BTN_BORDER_WIDTH / r.size.w,
		.y = param + RECT_BTN_BORDER_WIDTH / r.size.h,
	};
	Rect sr    = scale_rect_centered(r, (v2){.x = param, .y = param});
	Rect sb    = scale_rect_centered(r, bscale);
	DrawRectangleRounded(sb.rl, RECT_BTN_ROUNDNESS, 0, RECT_BTN_BORDER_COLOUR);
	DrawRectangleRounded(sr.rl,  RECT_BTN_ROUNDNESS, 0, RECT_BTN_COLOUR);

	v2 tpos   = center_align_text_in_rect(r, text, ctx->font);
	v2 spos   = {.x = tpos.x + 1.75, .y = tpos.y + 2};
	v4 colour = lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t);

	draw_text(ctx->font, text, spos, 0, fade(BLACK, 0.8));
	draw_text(ctx->font, text, tpos, 0, colour_from_normalized(colour));

	return pressed;
}

static void
draw_settings_ui(BeamformerCtx *ctx, Arena arena, Rect r, v2 mouse)
{
	if (IsKeyPressed(KEY_ENTER) && ctx->is.store.value)
		set_text_input_idx(ctx, (BPModifiableValue){0}, (Rect){0}, mouse);

	BeamformerParameters *bp = &ctx->params->raw;

	f32 minx = bp->output_min_coordinate.x + 1e-6, maxx = bp->output_max_coordinate.x - 1e-6;
	f32 minz = bp->output_min_coordinate.z + 1e-6, maxz = bp->output_max_coordinate.z - 1e-6;

	Rect draw_r    = r;
	draw_r.pos.y  += 20;
	draw_r.pos.x  += 20;
	draw_r.size.x -= 20;
	draw_r.size.y -= 20;

	draw_r = do_value_listing(s8("Sampling Frequency:"), s8("[MHz]"),
	                          bp->sampling_frequency * 1e-6, ctx->font, arena, draw_r);

	static f32 hover_t[13];
	i32 idx = 0;

	BPModifiableValue bmv;
	bmv = (BPModifiableValue){&bp->center_frequency, MV_FLOAT|MV_CAUSES_COMPUTE, 1e-6,
	                          .flimits = (v2){.y = 100e6}};
	draw_r = do_text_input_listing(s8("Center Frequency:"), s8("[MHz]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->speed_of_sound, MV_FLOAT|MV_CAUSES_COMPUTE, 1,
	                          .flimits = (v2){.y = 1e6}};
	draw_r = do_text_input_listing(s8("Speed of Sound:"), s8("[m/s]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->output_min_coordinate.x, MV_FLOAT|MV_CAUSES_COMPUTE, 1e3,
	                          .flimits = (v2){.x = -1e3, .y = maxx}};
	draw_r = do_text_input_listing(s8("Min Lateral Point:"), s8("[mm]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->output_max_coordinate.x, MV_FLOAT|MV_CAUSES_COMPUTE, 1e3,
	                          .flimits = (v2){.x = minx, .y = 1e3}};
	draw_r = do_text_input_listing(s8("Max Lateral Point:"), s8("[mm]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->output_min_coordinate.z, MV_FLOAT|MV_CAUSES_COMPUTE, 1e3,
	                         .flimits =  (v2){.y = maxz}};
	draw_r = do_text_input_listing(s8("Min Axial Point:"), s8("[mm]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->output_max_coordinate.z, MV_FLOAT|MV_CAUSES_COMPUTE, 1e3,
	                          .flimits = (v2){.x = minz, .y = 1e3}};
	draw_r = do_text_input_listing(s8("Max Axial Point:"), s8("[mm]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->off_axis_pos, MV_FLOAT|MV_CAUSES_COMPUTE, 1e3,
	                          .flimits = (v2){.x = -1, .y = 1}};
	draw_r = do_text_input_listing(s8("Off Axis Position:"), s8("[mm]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&bp->beamform_plane, MV_INT|MV_CAUSES_COMPUTE, 1,
	                          .ilimits = (iv2){.y = 1}};
	draw_r = do_text_toggle_listing(s8("Beamform Plane:"), s8("XZ"), s8("YZ"), bp->beamform_plane,
	                                bmv, ctx, draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&ctx->fsctx.db, MV_FLOAT|MV_GEN_MIPMAPS, 1,
	                          .flimits = (v2){.x = -120}};
	draw_r = do_text_input_listing(s8("Dynamic Range:"), s8("[dB]"), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	draw_r.pos.y  += 2 * LISTING_LINE_PAD;
	draw_r.size.y -= 2 * LISTING_LINE_PAD;

	bmv = (BPModifiableValue){&ctx->export_ctx.volume_dim.x, MV_INT|MV_POWER_OF_TWO, 1,
	                          .ilimits = (iv2){.x = 1, .y = 2048}};
	draw_r = do_text_input_listing(s8("Export Dimension X:"), s8(""), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&ctx->export_ctx.volume_dim.y, MV_INT|MV_POWER_OF_TWO, 1,
	                          .ilimits = (iv2){.x = 1, .y = 2048}};
	draw_r = do_text_input_listing(s8("Export Dimension Y:"), s8(""), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&ctx->export_ctx.volume_dim.z, MV_INT|MV_POWER_OF_TWO, 1,
	                          .ilimits = (iv2){.x = 1, .y = 2048}};
	draw_r = do_text_input_listing(s8("Export Dimension Z:"), s8(""), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	Rect btn_r = draw_r;
	btn_r.size.h  = ctx->font.baseSize * 1.3;
	btn_r.size.w *= 0.6;
	if (do_text_button(ctx, s8("Dump Raw Volume"), btn_r, mouse, hover_t + idx++)) {
		if (!ctx->export_ctx.state) {
			ctx->export_ctx.state  = ES_START;
			ctx->flags            |= DO_COMPUTE;
		}
	}

	/* NOTE: if C compilers didn't suck this would be a static assert */
	ASSERT(idx <= ARRAY_COUNT(hover_t));
}

static void
draw_debug_overlay(BeamformerCtx *ctx, Arena arena, Rect r)
{
	uv2 ws = ctx->window_size;

	static s8 labels[CS_LAST] = {
		[CS_CUDA_DECODE]  = s8("CUDA Decoding:"),
		[CS_CUDA_HILBERT] = s8("CUDA Hilbert:"),
		[CS_DEMOD]        = s8("Demodulation:"),
		[CS_HADAMARD]     = s8("Decoding:"),
		[CS_HERCULES]     = s8("HERCULES:"),
		[CS_MIN_MAX]      = s8("Min/Max:"),
		[CS_UFORCES]      = s8("UFORCES:"),
	};

	ComputeShaderCtx *cs = &ctx->csctx;

	s8 txt_buf = s8alloc(&arena, 64);
	v2 pos = {.x = 20, .y = ws.h - 10};

	f32 compute_time_sum = 0;
	u32 stages = ctx->params->compute_stages_count;
	for (u32 i = 0; i < stages; i++) {
		u32 index  = ctx->params->compute_stages[i];
		pos.y     -= measure_text(ctx->font, labels[index]).y;
		draw_text(ctx->font, labels[index], pos, 0, colour_from_normalized(FG_COLOUR));

		s8 tmp  = txt_buf;
		tmp.len = snprintf((char *)txt_buf.data, txt_buf.len, "%0.02e [s]",
		                   cs->last_frame_time[index]);
		v2 txt_fs = measure_text(ctx->font, tmp);
		v2 rpos   = {.x = r.pos.x + r.size.w - txt_fs.w, .y = pos.y};
		draw_text(ctx->font, tmp, rpos, 0, colour_from_normalized(FG_COLOUR));

		compute_time_sum += cs->last_frame_time[index];
	}

	static s8 totals[2] = {s8("Compute Total:"), s8("Volume Total:")};
	f32 times[2]        = {compute_time_sum, ctx->export_ctx.runtime};
	for (u32 i = 0; i < ARRAY_COUNT(totals); i++) {
		pos.y    -= measure_text(ctx->font, totals[i]).y;
		draw_text(ctx->font, totals[i], pos, 0, colour_from_normalized(FG_COLOUR));

		s8 tmp  = txt_buf;
		tmp.len = snprintf((char *)txt_buf.data, txt_buf.len, "%0.02e [s]", times[i]);
		v2 txt_fs = measure_text(ctx->font, tmp);
		v2 rpos   = {.x = r.pos.x + r.size.w - txt_fs.w, .y = pos.y};
		draw_text(ctx->font, tmp, rpos, 0, colour_from_normalized(FG_COLOUR));
	}

	{
		static v2 pos       = {.x = 32,  .y = 128};
		static v2 scale     = {.x = 1.0, .y = 1.0};
		static u32 txt_idx  = 0;
		static s8 txt[2]    = { s8("-_-"), s8("^_^") };
		static v2 ts[2];
		if (ts[0].x == 0) {
			ts[0] = measure_text(ctx->font, txt[0]);
			ts[1] = measure_text(ctx->font, txt[1]);
		}

		pos.x += 130 * ctx->dt * scale.x;
		pos.y += 120 * ctx->dt * scale.y;

		if (pos.x > (ws.w - ts[txt_idx].x) || pos.x < 0) {
			txt_idx = !txt_idx;
			CLAMP(pos.x, 0, ws.w - ts[txt_idx].x);
			scale.x *= -1.0;
		}

		if (pos.y > (ws.h - ts[txt_idx].y) || pos.y < 0) {
			txt_idx = !txt_idx;
			CLAMP(pos.y, 0, ws.h - ts[txt_idx].y);
			scale.y *= -1.0;
		}

		draw_text(ctx->font, txt[txt_idx], pos, 0, RED);
	}
}

static void
draw_ui(BeamformerCtx *ctx, Arena arena)
{
	BeamformerParameters *bp = &ctx->params->raw;

	BeginDrawing();
		ClearBackground(colour_from_normalized(BG_COLOUR));

		Texture *output   = &ctx->fsctx.output.texture;

		/* TODO: this depends on the direction being rendered (x vs y) */
		v2 output_dim = {
			.x = bp->output_max_coordinate.x - bp->output_min_coordinate.x,
			.y = bp->output_max_coordinate.z - bp->output_min_coordinate.z,
		};

		v2 mouse = { .rl = GetMousePosition() };
		Rect wr = {.size = {.w = (f32)ctx->window_size.w, .h = (f32)ctx->window_size.h}};
		Rect lr = wr, rr = wr;
		lr.size.w = INFO_COLUMN_WIDTH;
		rr.size.w = wr.size.w - lr.size.w;
		rr.pos.x  = lr.pos.x  + lr.size.w;

		if (output_dim.x > 1e-6 && output_dim.y > 1e-6) {
			s8 txt   = s8alloc(&arena, 64);
			s8 tmp   = txt;
			tmp.len  = snprintf((char *)txt.data, txt.len, "%+0.01f mm", -188.8f);
			v2 txt_s = measure_text(ctx->small_font, tmp);

			rr.pos.x  += 0.02 * rr.size.w;
			rr.pos.y  += 0.02 * rr.size.h;
			rr.size.w *= 0.96;
			rr.size.h *= 0.96;

			f32 tick_len = 20;
			f32 pad      = 1.2 * txt_s.x + tick_len;

			Rect vr    = rr;
			vr.pos.x  += 0.5 * txt_s.y;
			vr.pos.y  += 0.5 * txt_s.y;
			vr.size.h  = rr.size.h - pad;
			vr.size.w  = rr.size.w - pad;

			f32 aspect = output_dim.h / output_dim.w;
			if (rr.size.h < (vr.size.w * aspect) + pad) {
				vr.size.w = vr.size.h / aspect;
			} else {
				vr.size.h = vr.size.w * aspect;
			}
			vr.pos.x += (rr.size.w - (vr.size.w + pad)) / 2;
			vr.pos.y += (rr.size.h - (vr.size.h + pad)) / 2;

			Rectangle tex_r   = { 0.0f, 0.0f, (f32)output->width, -(f32)output->height };
			NPatchInfo tex_np = { tex_r, 0, 0, 0, 0, NPATCH_NINE_PATCH };
			DrawTextureNPatch(*output, tex_np, vr.rl, (Vector2){0}, 0, WHITE);

			/* NOTE: check mouse wheel for adjusting dynamic range of image */
			if (CheckCollisionPointRec(mouse.rl, vr.rl)) {
				f32 delta = GetMouseWheelMove();
				ctx->fsctx.db += delta;
				CLAMP(ctx->fsctx.db, -120, 0);
				if (delta) ctx->flags |= GEN_MIPMAPS;
			}

			static f32 txt_colour_t[2];
			for (u32 i = 0; i < 2; i++) {
				u32 line_count   = vr.size.E[i] / (1.5 * txt_s.h);
				f32 inc          = vr.size.E[i] / line_count;
				v2 start_pos     = vr.pos;
				start_pos.E[!i] += vr.size.E[!i];

				v2 end_pos       = start_pos;
				end_pos.E[!i]   += tick_len;

				/* NOTE: Center the Text with the Tick center */
				f32 txt_pos_scale[2] = {1, -1};
				v2 txt_pos  = end_pos;
				txt_pos.E[i]  += txt_pos_scale[i] * txt_s.y/2;
				txt_pos.E[!i] += 10;

				Rect tick_rect       = {.pos = start_pos, .size = vr.size};
				tick_rect.size.E[!i] = 10 + tick_len + txt_s.x;

				/* TODO: don't do this nonsense; this code will need to get
				 * split into a seperate function */
				u32 coord_idx = i == 0? 0 : 2;
				if (CheckCollisionPointRec(mouse.rl, tick_rect.rl)) {
					f32 scale[2]   = {0.5e-3, 1e-3};
					f32 size_delta = GetMouseWheelMove() * scale[i];
					/* TODO: smooth scroll this? */
					if (coord_idx== 0)
						bp->output_min_coordinate.E[coord_idx] -= size_delta;
					bp->output_max_coordinate.E[coord_idx] += size_delta;
					if (size_delta) {
						ctx->flags |= DO_COMPUTE;
						ctx->params->upload = 1;
					}

					txt_colour_t[i] += TEXT_HOVER_SPEED * ctx->dt;
				} else {
					txt_colour_t[i] -= TEXT_HOVER_SPEED * ctx->dt;
				}
				CLAMP01(txt_colour_t[i]);

				f32 mm     = bp->output_min_coordinate.E[coord_idx] * 1e3;
				f32 mm_inc = inc * output_dim.E[i] * 1e3 / vr.size.E[i];

				Color txt_colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR,
				                                                  txt_colour_t[i]));

				char *fmt[2] = {"%+0.01f mm", "%0.01f mm"};
				f32 rot[2] = {90, 0};
				for (u32 j = 0; j <= line_count; j++) {
					DrawLineEx(start_pos.rl, end_pos.rl, 3, colour_from_normalized(FG_COLOUR));
					s8 tmp = txt;
					tmp.len = snprintf((char *)txt.data, txt.len, fmt[i], mm);
					draw_text(ctx->small_font, tmp, txt_pos, rot[i], txt_colour);
					start_pos.E[i] += inc;
					end_pos.E[i]   += inc;
					txt_pos.E[i]   += inc;
					mm             += mm_inc;
				}
			}
		}

		draw_settings_ui(ctx, arena, lr, mouse);
		draw_debug_overlay(ctx, arena, lr);
	EndDrawing();
}
