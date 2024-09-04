/* See LICENSE for license details. */
#include "beamformer.h"

static Color
colour_from_normalized(v4 rgba)
{
	return (Color){.r = rgba.r * 255.0f, .g = rgba.g * 255.0f,
	               .b = rgba.b * 255.0f, .a = rgba.a * 255.0f};
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

static void
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

struct listing {
	s8   prefix;
	s8   suffix;
	f32  *data;
	v2   limits;
	f32  data_scale;
	b32  editable;
};

static void
parse_and_store_text_input(BeamformerCtx *ctx, struct listing *l)
{
	f32 new_val = strtof(ctx->is.buf, NULL);
	/* TODO: allow zero for certain listings only */
	if (new_val / l->data_scale != *l->data) {
		*l->data = new_val / l->data_scale;
		CLAMP(*l->data, l->limits.x, l->limits.y);
		ctx->flags |= DO_COMPUTE;
		ctx->params->upload = 1;
	}
}

static void
set_text_input_idx(BeamformerCtx *ctx, i32 idx, struct listing *l, struct listing *last_l, Rect r,
                   v2 mouse)
{
	if (ctx->is.idx != idx && ctx->is.idx != -1)
		parse_and_store_text_input(ctx, last_l);

	ctx->is.buf_len = snprintf(ctx->is.buf, ARRAY_COUNT(ctx->is.buf), "%0.02f",
	                           *l->data * l->data_scale);

	ctx->is.idx    = idx;
	ctx->is.cursor = -1;

	if (ctx->is.idx == -1)
		return;

	ASSERT(CheckCollisionPointRec(mouse.rl, r.rl));
	ctx->is.cursor_hover_p = (mouse.x - r.pos.x) / r.size.w;
	CLAMP01(ctx->is.cursor_hover_p);
}

static void
draw_settings_ui(BeamformerCtx *ctx, Arena arena, Rect r, v2 mouse)
{
	BeamformerParameters *bp = &ctx->params->raw;

	f32 minx = bp->output_min_xz.x + 1e-6, maxx = bp->output_max_xz.x - 1e-6;
	f32 minz = bp->output_min_xz.y + 1e-6, maxz = bp->output_max_xz.y - 1e-6;
	struct listing listings[] = {
		{ s8("Sampling Rate:"),    s8("[MHz]"), &bp->sampling_frequency, {{0,    0}},        1e-6, 0 },
		{ s8("Center Frequency:"), s8("[MHz]"), &bp->center_frequency,   {{0,    100e6}},    1e-6, 1 },
		{ s8("Speed of Sound:"),   s8("[m/s]"), &bp->speed_of_sound,     {{0,    1e6}},      1,    1 },
		{ s8("Min X Point:"),      s8("[mm]"),  &bp->output_min_xz.x,    {{-1e3, maxx}},     1e3,  1 },
		{ s8("Max X Point:"),      s8("[mm]"),  &bp->output_max_xz.x,    {{minx, 1e3}},      1e3,  1 },
		{ s8("Min Z Point:"),      s8("[mm]"),  &bp->output_min_xz.y,    {{0,    maxz}},     1e3,  1 },
		{ s8("Max Z Point:"),      s8("[mm]"),  &bp->output_max_xz.y,    {{minz, 1e3}},      1e3,  1 },
		{ s8("Dynamic Range:"),    s8("[dB]"),  &ctx->fsctx.db,          {{-120, 0}},        1,    1 },
		{ s8("Y Position:"),       s8("[mm]"),  &bp->off_axis_pos,       {{minx*2, maxx*2}}, 1e3,  1 },
	};

	static f32 hover_t[ARRAY_COUNT(listings)];

	f32 line_pad  = 10;

	v2 pos  = r.pos;
	pos.y  += 50;
	pos.x  += 20;

	s8 txt = s8alloc(&arena, 64);
	f32 max_prefix_len = 0;
	for (i32 i = 0; i < ARRAY_COUNT(listings); i++) {
		struct listing *l = listings + i;
		draw_text(ctx->font, l->prefix, pos, 0, colour_from_normalized(FG_COLOUR));
		v2 prefix_s = measure_text(ctx->font, l->prefix);
		if (prefix_s.w > max_prefix_len)
			max_prefix_len = prefix_s.w;
		pos.y += prefix_s.y + line_pad;
	}
	pos.y = 50 + r.pos.y;

	for (i32 i = 0; i < ARRAY_COUNT(listings); i++) {
		struct listing *l = listings + i;

		s8 tmp_s = txt;
		v2 txt_s;
		if (ctx->is.idx == i) {
			txt_s = measure_text(ctx->font, (s8){.len = ctx->is.buf_len,
			                                     .data = (u8 *)ctx->is.buf});
		} else {
			tmp_s.len = snprintf((char *)txt.data, txt.len, "%0.02f", *l->data * l->data_scale);
			txt_s     = measure_text(ctx->font, tmp_s);
		}

		Rect edit_rect = {
			.pos  = {.x = pos.x + max_prefix_len + 15, .y = pos.y},
			.size = {.x = txt_s.w + TEXT_BOX_EXTRA_X,  .y = txt_s.h}
		};

		b32 collides = CheckCollisionPointRec(mouse.rl, edit_rect.rl);
		if (collides && l->editable) {
			f32 mouse_scroll = GetMouseWheelMove();
			if (mouse_scroll) {
				*l->data += mouse_scroll / l->data_scale;
				CLAMP(*l->data, l->limits.x, l->limits.y);
				ctx->flags |= DO_COMPUTE;
				ctx->params->upload = 1;
			}
		}

		if (collides && ctx->is.idx != i && l->editable)
			hover_t[i] += TEXT_HOVER_SPEED * ctx->dt;
		else
			hover_t[i] -= TEXT_HOVER_SPEED * ctx->dt;
		CLAMP01(hover_t[i]);

		if (!collides && ctx->is.idx == i && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
			set_text_input_idx(ctx, -1, l, l, (Rect){0}, mouse);
			tmp_s.len = snprintf((char *)txt.data, txt.len, "%0.02f", *l->data * l->data_scale);
		}

		if (collides && l->editable && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
			set_text_input_idx(ctx, i, l, listings + ctx->is.idx, edit_rect, mouse);

		Color colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, hover_t[i]));

		if (ctx->is.idx != i) {
			draw_text(ctx->font, tmp_s, edit_rect.pos, 0, colour);
		} else {
			do_text_input(ctx, 7, edit_rect, colour);
		}

		v2 suffix_s = measure_text(ctx->font, l->suffix);
		v2 suffix_p = {.x = r.pos.x + r.size.w - suffix_s.w, .y = pos.y};
		draw_text(ctx->font, l->suffix, suffix_p, 0, colour_from_normalized(FG_COLOUR));
		pos.y += txt_s.y + line_pad;
	}

	if (IsKeyPressed(KEY_ENTER) && ctx->is.idx != -1) {
		struct listing *l = listings + ctx->is.idx;
		set_text_input_idx(ctx, -1, l, l, (Rect){0}, mouse);
	}
}



static void
draw_debug_overlay(BeamformerCtx *ctx, Arena arena, Rect r)
{
	DrawFPS(20, 20);

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

	{
		s8 label  = s8("Compute Total:");
		pos.y    -= measure_text(ctx->font, label).y;
		draw_text(ctx->font, label, pos, 0, colour_from_normalized(FG_COLOUR));

		s8 tmp  = txt_buf;
		tmp.len = snprintf((char *)txt_buf.data, txt_buf.len, "%0.02e [s]",
		                   compute_time_sum);
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

		v2 output_dim = {
			.x = bp->output_max_xz.x - bp->output_min_xz.x,
			.y = bp->output_max_xz.y - bp->output_min_xz.y,
		};

		v2 mouse = { .rl = GetMousePosition() };
		Rect wr = {.size = {.w = (f32)ctx->window_size.w, .h = (f32)ctx->window_size.h}};
		Rect lr = wr, rr = wr;
		lr.size.w = 420;

		if (output_dim.x > 1e-6 && output_dim.y > 1e-6) {
			s8 txt   = s8alloc(&arena, 64);
			s8 tmp   = txt;
			tmp.len  = snprintf((char *)txt.data, txt.len, "%+0.01f mm", -88.8f);
			v2 txt_s = measure_text(ctx->font, tmp);

			rr.size.w  = wr.size.w - lr.size.w;
			rr.pos.x   = lr.pos.x  + lr.size.w;

			rr.pos.x  += 0.07 * rr.size.w;
			rr.size.w *= 0.86;
			rr.pos.y  += 0.02 * rr.size.h;
			rr.size.h *= 0.96;

			f32 tick_len = 20;
			f32 x_pad    = 1.0  * txt_s.x + tick_len + 0.5 * txt_s.y;
			f32 y_pad    = 1.25 * txt_s.x + tick_len;

			Rect vr    = rr;
			vr.size.h -= y_pad;
			vr.size.w  = vr.size.h * output_dim.w / output_dim.h - x_pad;
			if (vr.size.w + x_pad > rr.size.w) {
				vr.size.h = (rr.size.w - x_pad) * output_dim.h / output_dim.w;
				vr.size.w = vr.size.h * output_dim.w / output_dim.h;
			}
			vr.pos.x += (rr.size.w - (vr.size.w + x_pad) + txt_s.h) / 2;
			vr.pos.y += (rr.size.h - (vr.size.h + y_pad) + txt_s.h) / 2;
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
				u32 line_count   = vr.size.E[i] / txt_s.h;
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

				if (CheckCollisionPointRec(mouse.rl, tick_rect.rl)) {
					f32 scale[2]   = {0.5e-3, 1e-3};
					f32 size_delta = GetMouseWheelMove() * scale[i];
					/* TODO: smooth scroll this? */
					if (i == 0)
						bp->output_min_xz.E[i] -= size_delta;
					bp->output_max_xz.E[i] += size_delta;
					if (size_delta) {
						ctx->flags |= DO_COMPUTE;
						ctx->params->upload = 1;
					}

					txt_colour_t[i] += TEXT_HOVER_SPEED * ctx->dt;
				} else {
					txt_colour_t[i] -= TEXT_HOVER_SPEED * ctx->dt;
				}
				CLAMP01(txt_colour_t[i]);

				f32 mm     = bp->output_min_xz.E[i] * 1e3;
				f32 mm_inc = inc * output_dim.E[i] * 1e3 / vr.size.E[i];

				Color txt_colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR,
				                                                  txt_colour_t[i]));

				char *fmt[2] = {"%+0.01f mm", "%0.01f mm"};
				f32 rot[2] = {90, 0};
				for (u32 j = 0; j <= line_count; j++) {
					DrawLineEx(start_pos.rl, end_pos.rl, 3, colour_from_normalized(FG_COLOUR));
					s8 tmp = txt;
					tmp.len = snprintf((char *)txt.data, txt.len, fmt[i], mm);
					draw_text(ctx->font, tmp, txt_pos, rot[i], txt_colour);
					start_pos.E[i] += inc;
					end_pos.E[i]   += inc;
					txt_pos.E[i]   += inc;
					mm             += mm_inc;
				}
			}

			/* TODO: this should be removed */
			f32 desired_width = lr.size.w + vr.size.w;
			if (desired_width > ctx->window_size.w) {
				ctx->window_size.w = desired_width;
				SetWindowSize(ctx->window_size.w, ctx->window_size.h);
				SetWindowMinSize(desired_width, 720);
			}
		}

		draw_settings_ui(ctx, arena, lr, mouse);
		draw_debug_overlay(ctx, arena, lr);
	EndDrawing();
}
