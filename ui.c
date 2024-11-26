/* See LICENSE for license details. */
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
hover_text(v2 mouse, Rect text_rect, f32 *hover_t, b32 can_advance)
{
	b32 hovering = CheckCollisionPointRec(mouse.rl, text_rect.rl);
	if (hovering && can_advance) *hover_t += TEXT_HOVER_SPEED * dt_for_frame;
	else                         *hover_t -= TEXT_HOVER_SPEED * dt_for_frame;
	*hover_t = CLAMP01(*hover_t);
	return hovering;
}

/* NOTE: This is kinda sucks no matter how you spin it. If we want the values to be
 * left aligned in the center column we need to know the longest prefix length but
 * without either hardcoding one of the prefixes as the longest one or measuring all
 * of them we can't know this ahead of time. For now we hardcode this and manually
 * adjust when needed */
#define LISTING_LEFT_COLUMN_WIDTH 270.0f
#define LISTING_LINE_PAD           6.0f

static Rect
do_value_listing(s8 prefix, s8 suffix, Arena a, f32 value, Font font, Rect r)
{
	v2 suffix_s = measure_text(font, suffix);
	v2 suffix_p = {.x = r.pos.x + r.size.w - suffix_s.w, .y = r.pos.y};

	Stream buf = arena_stream(&a);
	stream_append_f64(&buf, value, 100);
	v2 txt_p = {.x = r.pos.x + LISTING_LEFT_COLUMN_WIDTH, .y = r.pos.y};

	draw_text(font, prefix,             r.pos,    0, colour_from_normalized(FG_COLOUR));
	draw_text(font, stream_to_s8(&buf), txt_p,    0, colour_from_normalized(FG_COLOUR));
	draw_text(font, suffix,             suffix_p, 0, colour_from_normalized(FG_COLOUR));
	r.pos.y  += suffix_s.h + LISTING_LINE_PAD;
	r.size.y -= suffix_s.h + LISTING_LINE_PAD;

	return r;
}

static Rect
do_text_input_listing(s8 prefix, s8 suffix, Variable var, BeamformerUI *ui, Rect r,
                      v2 mouse, f32 *hover_t)
{
	InputState   *is = &ui->text_input_state;
	b32 text_input_active = (ui->interaction.state == IS_TEXT) &&
	                        (var.store == ui->interaction.active.store);

	Arena  arena = ui->arena_for_frame;
	Stream buf   = arena_stream(&arena);
	v2 txt_s;

	if (text_input_active) {
		txt_s = measure_text(ui->font, (s8){.len = is->buf_len, .data = is->buf});
	} else {
		stream_append_variable(&buf, &var);
		txt_s = measure_text(ui->font, stream_to_s8(&buf));
	}

	Rect edit_rect = {
		.pos  = {.x = r.pos.x + LISTING_LEFT_COLUMN_WIDTH, .y = r.pos.y},
		.size = {.x = txt_s.w + TEXT_BOX_EXTRA_X, .y = txt_s.h}
	};

	b32 hovering = hover_text(mouse, edit_rect, hover_t, !text_input_active);
	if (hovering)
		ui->interaction.hot = var;

	/* TODO: where should this go? */
	if (text_input_active && is->cursor == -1) {
		/* NOTE: extra offset to help with putting a cursor at idx 0 */
		#define TEXT_HALF_CHAR_WIDTH 10
		f32 hover_p = CLAMP01((mouse.x - edit_rect.pos.x) / edit_rect.size.w);
		f32 x_off = TEXT_HALF_CHAR_WIDTH, x_bounds = edit_rect.size.w * hover_p;
		i32 i;
		for (i = 0; i < is->buf_len && x_off < x_bounds; i++) {
			/* NOTE: assumes font glyphs are ordered ASCII */
			i32 idx  = is->buf[i] - 0x20;
			x_off   += ui->font.glyphs[idx].advanceX;
			if (ui->font.glyphs[idx].advanceX == 0)
				x_off += ui->font.recs[idx].width;
		}
		is->cursor = i;
	}

	Color colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t));

	if (text_input_active) {
		s8 buf = {.len = is->buf_len, .data = is->buf};
		v2 ts  = measure_text(ui->font, buf);
		v2 pos = {.x = edit_rect.pos.x, .y = edit_rect.pos.y + (edit_rect.size.y - ts.y) / 2};

		#define MAX_DISP_CHARS 7
		i32 buf_delta = is->buf_len - MAX_DISP_CHARS;
		if (buf_delta < 0) buf_delta = 0;
		buf.len  -= buf_delta;
		buf.data += buf_delta;
		{
			/* NOTE: drop a char if the subtext still doesn't fit */
			v2 nts = measure_text(ui->font, buf);
			if (nts.w > 0.96 * edit_rect.size.w) {
				buf.data++;
				buf.len--;
			}
		}
		draw_text(ui->font, buf, pos, 0, colour);

		v4 bg = FOCUSED_COLOUR;
		bg.a  = 0;
		Color cursor_colour = colour_from_normalized(lerp_v4(bg, FOCUSED_COLOUR,
		                                                     CLAMP01(is->cursor_blink_t)));
		buf.len = is->cursor - buf_delta;
		v2 sts = measure_text(ui->font, buf);
		f32 cursor_x = pos.x + sts.x;
		f32 cursor_width;
		if (is->cursor == is->buf_len) cursor_width = 20;
		else                           cursor_width = 4;
		Rect cursor_r = {
			.pos  = {.x = cursor_x,     .y = pos.y},
			.size = {.w = cursor_width, .h = ts.h},
		};

		DrawRectanglePro(cursor_r.rl, (Vector2){0}, 0, cursor_colour);
	} else {
		draw_text(ui->font, stream_to_s8(&buf), edit_rect.pos, 0, colour);
	}

	v2 suffix_s = measure_text(ui->font, suffix);
	v2 suffix_p = {.x = r.pos.x + r.size.w - suffix_s.w, .y = r.pos.y};
	draw_text(ui->font, prefix, r.pos,    0, colour_from_normalized(FG_COLOUR));
	draw_text(ui->font, suffix, suffix_p, 0, colour_from_normalized(FG_COLOUR));

	r.pos.y  += suffix_s.h + LISTING_LINE_PAD;
	r.size.y -= suffix_s.h + LISTING_LINE_PAD;

	return r;
}

static Rect
do_text_toggle_listing(s8 prefix, s8 text0, s8 text1, Variable var,
                       BeamformerUI *ui, Rect r, v2 mouse, f32 *hover_t)
{
	b32 toggle = *(b32 *)var.store;
	v2 txt_s;
	if (toggle) txt_s = measure_text(ui->font, text1);
	else        txt_s = measure_text(ui->font, text0);

	Rect edit_rect = {
		.pos  = {.x = r.pos.x + LISTING_LEFT_COLUMN_WIDTH, .y = r.pos.y},
		.size = {.x = txt_s.w + TEXT_BOX_EXTRA_X, .y = txt_s.h}
	};

	if (hover_text(mouse, edit_rect, hover_t, 1))
		ui->interaction.hot = var;

	Color colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t));
	draw_text(ui->font, prefix, r.pos, 0, colour_from_normalized(FG_COLOUR));
	draw_text(ui->font, toggle? text1: text0, edit_rect.pos, 0, colour);

	r.pos.y  += txt_s.h + LISTING_LINE_PAD;
	r.size.y -= txt_s.h + LISTING_LINE_PAD;

	return r;
}

static b32
do_text_button(BeamformerUI *ui, s8 text, Rect r, v2 mouse, f32 *hover_t)
{
	b32 hovering = hover_text(mouse, r, hover_t, 1);
	b32 pressed  = 0;
	pressed     |= (hovering && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
	pressed     |= (hovering && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT));

	f32 param  = lerp(1, 1.04, *hover_t);
	v2  bscale = (v2){
		.x = param + RECT_BTN_BORDER_WIDTH / r.size.w,
		.y = param + RECT_BTN_BORDER_WIDTH / r.size.h,
	};
	Rect sr    = scale_rect_centered(r, (v2){.x = param, .y = param});
	Rect sb    = scale_rect_centered(r, bscale);
	DrawRectangleRounded(sb.rl, RECT_BTN_ROUNDNESS, 0, RECT_BTN_BORDER_COLOUR);
	DrawRectangleRounded(sr.rl, RECT_BTN_ROUNDNESS, 0, RECT_BTN_COLOUR);

	v2 tpos   = center_align_text_in_rect(r, text, ui->font);
	v2 spos   = {.x = tpos.x + 1.75, .y = tpos.y + 2};
	v4 colour = lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t);

	draw_text(ui->font, text, spos, 0, fade(BLACK, 0.8));
	draw_text(ui->font, text, tpos, 0, colour_from_normalized(colour));

	return pressed;
}

static void
draw_settings_ui(BeamformerCtx *ctx, Rect r, v2 mouse)
{
	BeamformerUI *ui         = ctx->ui;
	BeamformerParameters *bp = &ctx->params->raw;

	f32 minx = bp->output_min_coordinate.x + 1e-6, maxx = bp->output_max_coordinate.x - 1e-6;
	f32 minz = bp->output_min_coordinate.z + 1e-6, maxz = bp->output_max_coordinate.z - 1e-6;

	Rect draw_r    = r;
	draw_r.pos.y  += 20;
	draw_r.pos.x  += 20;
	draw_r.size.x -= 20;
	draw_r.size.y -= 20;

	draw_r = do_value_listing(s8("Sampling Frequency:"), s8("[MHz]"), ui->arena_for_frame,
	                          bp->sampling_frequency * 1e-6, ui->font, draw_r);

	static f32 hover_t[15];
	i32 idx = 0;

	Variable var;

	var.store         = &bp->center_frequency;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.y = 100e6};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1e-6;
	var.scroll_scale  = 1e5;
	draw_r = do_text_input_listing(s8("Center Frequency:"), s8("[MHz]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &bp->speed_of_sound;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.y = 1e6};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1;
	var.scroll_scale  = 10;
	draw_r = do_text_input_listing(s8("Speed of Sound:"), s8("[m/s]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &bp->output_min_coordinate.x;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.x = -1e3, .y = maxx};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1e3;
	var.scroll_scale  = 0.5e-3;
	draw_r = do_text_input_listing(s8("Min Lateral Point:"), s8("[mm]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &bp->output_max_coordinate.x;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.x = minx, .y = 1e3};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1e3;
	var.scroll_scale  = 0.5e-3;
	draw_r = do_text_input_listing(s8("Max Lateral Point:"), s8("[mm]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &bp->output_min_coordinate.z;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.y = maxz};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1e3;
	var.scroll_scale  = 0.5e-3;
	draw_r = do_text_input_listing(s8("Min Axial Point:"), s8("[mm]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &bp->output_max_coordinate.z;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.x = minz, .y = 1e3};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1e3;
	var.scroll_scale  = 0.5e-3;
	draw_r = do_text_input_listing(s8("Max Axial Point:"), s8("[mm]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &bp->off_axis_pos;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.x = minx, .y = maxx};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1e3;
	var.scroll_scale  = 0.5e-3;
	draw_r = do_text_input_listing(s8("Off Axis Position:"), s8("[mm]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var       = (Variable){0};
	var.store = &bp->beamform_plane;
	var.type  = VT_B32;
	var.flags = V_CAUSES_COMPUTE;
	draw_r = do_text_toggle_listing(s8("Beamform Plane:"), s8("XZ"), s8("YZ"), var, ui,
	                                draw_r, mouse, hover_t + idx++);

	var.store         = &bp->f_number;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.y = 1e3};
	var.flags         = V_CAUSES_COMPUTE;
	var.display_scale = 1;
	var.scroll_scale  = 0.1;
	draw_r = do_text_input_listing(s8("F#:"), s8(""), var, ui, draw_r, mouse, hover_t + idx++);

	var.store         = &ctx->fsctx.db;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.x = -120};
	var.flags         = V_GEN_MIPMAPS;
	var.display_scale = 1;
	var.scroll_scale  = 1;
	draw_r = do_text_input_listing(s8("Dynamic Range:"), s8("[dB]"), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	var.store         = &ctx->fsctx.threshold;
	var.type          = VT_F32;
	var.f32_limits    = (v2){.y = 240};
	var.flags         = V_GEN_MIPMAPS;
	var.display_scale = 1;
	var.scroll_scale  = 1;
	draw_r = do_text_input_listing(s8("Threshold:"), s8(""), var, ui, draw_r,
	                               mouse, hover_t + idx++);

	draw_r.pos.y  += 2 * LISTING_LINE_PAD;
	draw_r.size.y -= 2 * LISTING_LINE_PAD;

	#if 0
	/* TODO: work this into the work queue */
	bmv = (BPModifiableValue){&ctx->partial_compute_ctx.volume_dim.x, bmv_store_power_of_two,
	                          .ilimits = (iv2){.x = 1, .y = ctx->gl.max_3d_texture_dim},
	                          MV_INT, 1, 1};
	draw_r = do_text_input_listing(s8("Export Dimension X:"), s8(""), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&ctx->partial_compute_ctx.volume_dim.y, bmv_store_power_of_two,
	                          .ilimits = (iv2){.x = 1, .y = ctx->gl.max_3d_texture_dim},
	                          MV_INT, 1, 1};
	draw_r = do_text_input_listing(s8("Export Dimension Y:"), s8(""), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	bmv = (BPModifiableValue){&ctx->partial_compute_ctx.volume_dim.z, bmv_store_power_of_two,
	                          .ilimits = (iv2){.x = 1, .y = ctx->gl.max_3d_texture_dim},
	                          MV_INT, 1, 1};
	draw_r = do_text_input_listing(s8("Export Dimension Z:"), s8(""), bmv, ctx, arena,
	                               draw_r, mouse, hover_t + idx++);

	Rect btn_r = draw_r;
	btn_r.size.h  = ctx->font.baseSize * 1.3;
	btn_r.size.w *= 0.6;
	if (do_text_button(ctx, s8("Dump Raw Volume"), btn_r, mouse, hover_t + idx++)) {
		if (!ctx->partial_compute_ctx.state) {
		}
	}
	#endif

	/* NOTE: if C compilers didn't suck this would be a static assert */
	ASSERT(idx <= ARRAY_COUNT(hover_t));
}

static void
draw_debug_overlay(BeamformerCtx *ctx, Arena arena, Rect r)
{
	static s8 labels[CS_LAST] = {
		[CS_CUDA_DECODE]  = s8("CUDA Decoding:"),
		[CS_CUDA_HILBERT] = s8("CUDA Hilbert:"),
		[CS_DAS]          = s8("DAS:"),
		[CS_DEMOD]        = s8("Demodulation:"),
		[CS_HADAMARD]     = s8("Decoding:"),
		[CS_MIN_MAX]      = s8("Min/Max:"),
		[CS_SUM]          = s8("Sum:"),
	};

	BeamformerUI *ui     = ctx->ui;
	ComputeShaderCtx *cs = &ctx->csctx;
	uv2 ws = ctx->window_size;

	Stream buf = stream_alloc(&arena, 64);
	v2 pos     = {.x = 20, .y = ws.h - 10};

	f32 compute_time_sum = 0;
	u32 stages = ctx->params->compute_stages_count;
	for (u32 i = 0; i < stages; i++) {
		u32 index  = ctx->params->compute_stages[i];
		pos.y     -= measure_text(ui->font, labels[index]).y;
		draw_text(ui->font, labels[index], pos, 0, colour_from_normalized(FG_COLOUR));

		buf.widx = 0;
		stream_append_f64_e(&buf, cs->last_frame_time[index]);
		stream_append_s8(&buf, s8(" [s]"));
		v2 txt_fs = measure_text(ui->font, stream_to_s8(&buf));
		v2 rpos   = {.x = r.pos.x + r.size.w - txt_fs.w, .y = pos.y};
		draw_text(ui->font, stream_to_s8(&buf), rpos, 0, colour_from_normalized(FG_COLOUR));

		compute_time_sum += cs->last_frame_time[index];
	}

	static s8 totals[2] = {s8("Compute Total:"), s8("Volume Total:")};
	f32 times[2]        = {compute_time_sum, ctx->partial_compute_ctx.runtime};
	for (u32 i = 0; i < ARRAY_COUNT(totals); i++) {
		pos.y    -= measure_text(ui->font, totals[i]).y;
		draw_text(ui->font, totals[i], pos, 0, colour_from_normalized(FG_COLOUR));

		buf.widx = 0;
		stream_append_f64_e(&buf, times[i]);
		stream_append_s8(&buf, s8(" [s]"));
		v2 txt_fs = measure_text(ui->font, stream_to_s8(&buf));
		v2 rpos   = {.x = r.pos.x + r.size.w - txt_fs.w, .y = pos.y};
		draw_text(ui->font, stream_to_s8(&buf), rpos, 0, colour_from_normalized(FG_COLOUR));
	}

	{
		static v2 pos       = {.x = 32,  .y = 128};
		static v2 scale     = {.x = 1.0, .y = 1.0};
		static u32 txt_idx  = 0;
		static s8 txt[2]    = { s8("-_-"), s8("^_^") };
		static v2 ts[2];
		if (ts[0].x == 0) {
			ts[0] = measure_text(ui->font, txt[0]);
			ts[1] = measure_text(ui->font, txt[1]);
		}

		pos.x += 130 * dt_for_frame * scale.x;
		pos.y += 120 * dt_for_frame * scale.y;

		if (pos.x > (ws.w - ts[txt_idx].x) || pos.x < 0) {
			txt_idx  = !txt_idx;
			pos.x    = CLAMP(pos.x, 0, ws.w - ts[txt_idx].x);
			scale.x *= -1.0;
		}

		if (pos.y > (ws.h - ts[txt_idx].y) || pos.y < 0) {
			txt_idx  = !txt_idx;
			pos.y    = CLAMP(pos.y, 0, ws.h - ts[txt_idx].y);
			scale.y *= -1.0;
		}

		draw_text(ui->font, txt[txt_idx], pos, 0, RED);
	}
}

static void
ui_store_variable(Variable *var, void *new_value)
{
	/* TODO: special cases (eg. power of 2) */
	switch (var->type) {
	case VT_F32: {
		f32  f32_val = *(f32 *)new_value;
		f32 *f32_var = var->store;
		*f32_var     = CLAMP(f32_val, var->f32_limits.x, var->f32_limits.y);
	} break;
	case VT_I32: {
		i32  i32_val = *(i32 *)new_value;
		i32 *i32_var = var->store;
		*i32_var     = CLAMP(i32_val, var->i32_limits.x, var->i32_limits.y);
	} break;
	default: INVALID_CODE_PATH;
	}
}

static void
begin_text_input(InputState *is, Variable *var)
{
	ASSERT(var->store != NULL);

	Stream s = {.cap = ARRAY_COUNT(is->buf), .data = is->buf};
	stream_append_variable(&s, var);
	ASSERT(!s.errors);
	is->buf_len = s.widx;
	is->cursor  = -1;
}

static void
end_text_input(InputState *is, Variable *var)
{
	f32 value = parse_f64((s8){.len = is->buf_len, .data = is->buf}) / var->display_scale;
	ui_store_variable(var, &value);
}

static void
update_text_input(InputState *is)
{
	if (is->cursor == -1)
		return;

	is->cursor_blink_t += is->cursor_blink_scale * dt_for_frame;
	if (is->cursor_blink_t >= 1) is->cursor_blink_scale = -1.5f;
	if (is->cursor_blink_t <= 0) is->cursor_blink_scale =  1.5f;

	/* NOTE: handle multiple input keys on a single frame */
	i32 key = GetCharPressed();
	while (key > 0) {
		if (is->buf_len == ARRAY_COUNT(is->buf))
			break;

		b32 allow_key = ((key >= '0' && key <= '9') || (key == '.') ||
		                 (key == '-' && is->cursor == 0));
		if (allow_key) {
			mem_move(is->buf + is->cursor,
			         is->buf + is->cursor + 1,
			         is->buf_len - is->cursor + 1);

			is->buf[is->cursor++] = key;
			is->buf_len++;
		}
		key = GetCharPressed();
	}

	if ((IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) && is->cursor > 0)
		is->cursor--;

	if ((IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) && is->cursor < is->buf_len)
		is->cursor++;

	if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && is->cursor > 0) {
		is->cursor--;
		mem_move(is->buf + is->cursor + 1,
		         is->buf + is->cursor,
		         is->buf_len - is->cursor);
		is->buf_len--;
	}

	if ((IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) && is->cursor < is->buf_len) {
		mem_move(is->buf + is->cursor + 1,
		         is->buf + is->cursor,
		         is->buf_len - is->cursor);
		is->buf_len--;
	}
}

static void
ui_start_compute(BeamformerCtx *ctx)
{
	/* NOTE: we do not allow ui to start a work if no work was previously completed */
	Arena a = {0};
	beamform_work_queue_push(ctx, &a, BW_RECOMPUTE);
	for (u32 i = 0; i < ARRAY_COUNT(ctx->beamform_frames); i++) {
		BeamformFrame *frame = ctx->beamform_frames + i;
		if (frame->dim.w && frame->textures[frame->dim.w - 1])
			glClearTexImage(frame->textures[frame->dim.w - 1], 0, GL_RED, GL_FLOAT, 0);
	}
	ctx->params->upload = 1;
}

static void
ui_gen_mipmaps(BeamformerCtx *ctx)
{
	if (ctx->fsctx.output.texture.id)
		ctx->flags |= GEN_MIPMAPS;
}

static void
ui_begin_interact(BeamformerUI *ui, BeamformerInput *input, b32 scroll)
{
	InteractionState *is = &ui->interaction;
	if (is->hot_state != IS_NONE) {
		is->state = is->hot_state;
	} else {
		switch (is->hot.type) {
		case VT_NULL:  is->state = IS_NOP; break;
		case VT_B32:   is->state = IS_SET; break;
		case VT_GROUP: is->state = IS_SET; break;
		case VT_F32: {
			if (scroll) {
				is->state = IS_SCROLL;
			} else {
				is->state = IS_TEXT;
				begin_text_input(&ui->text_input_state, &is->hot);
			}
		} break;
		}
	}
	if (is->state != IS_NONE) {
		is->active = is->hot;
	}
}

static void
ui_end_interact(BeamformerCtx *ctx, BeamformerUI *ui)
{
	InteractionState *is = &ui->interaction;
	switch (is->state) {
	case IS_NONE: break;
	case IS_NOP:  break;
	case IS_SET: {
		switch (is->active.type) {
		case VT_B32: {
			b32 *val = is->active.store;
			*val = !(*val);
		} break;
		}
	} break;
	case IS_DISPLAY:
		is->last_mouse_click_p = (v2){0};
		/* FALLTHROUGH */
	case IS_SCROLL: {
		f32 delta = GetMouseWheelMove() * is->active.scroll_scale;
		switch (is->active.type) {
		case VT_B32: {
			b32 *old_val = is->active.store;
			b32  new_val = !(*old_val);
			ui_store_variable(&is->active, &new_val);
		} break;
		case VT_F32: {
			f32 *old_val = is->active.store;
			f32  new_val = *old_val + delta;
			ui_store_variable(&is->active, &new_val);
		} break;
		case VT_I32: {
			i32 *old_val = is->active.store;
			i32  new_val = *old_val + delta;
			ui_store_variable(&is->active, &new_val);
		} break;
		}
	} break;
	case IS_TEXT: end_text_input(&ui->text_input_state, &is->active); break;
	}

	if (is->active.flags & V_CAUSES_COMPUTE)
		ui_start_compute(ctx);

	if (is->active.flags & V_GEN_MIPMAPS)
		ui_gen_mipmaps(ctx);

	is->state  = IS_NONE;
	is->active = NULL_VARIABLE;
}

static void
ui_interact(BeamformerCtx *ctx, BeamformerInput *input)
{
	BeamformerUI *ui       = ctx->ui;
	InteractionState *is   = &ui->interaction;
	b32 mouse_left_pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
	b32 wheel_moved        = GetMouseWheelMove();
	if (mouse_left_pressed || wheel_moved) {
		if (is->state != IS_NONE)
			ui_end_interact(ctx, ui);
		ui_begin_interact(ui, input, wheel_moved);
	}

	if (IsKeyPressed(KEY_ENTER) && is->state == IS_TEXT)
		ui_end_interact(ctx, ui);

	switch (is->state) {
	case IS_DISPLAY: {
		b32 should_end  = wheel_moved || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
		                  (is->active.store != is->hot.store);
		if (should_end) {
			ui_end_interact(ctx, ui);
		} else if (mouse_left_pressed) {
			is->last_mouse_click_p = input->mouse;
		}
	} break;
	case IS_SCROLL:  ui_end_interact(ctx, ui); break;
	case IS_SET:     ui_end_interact(ctx, ui); break;
	case IS_TEXT:    update_text_input(&ui->text_input_state); break;
	case IS_DRAG: {
		if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
			ui_end_interact(ctx, ui);
		} else {
			switch (is->active.type) {
			}
		}
	} break;
	}

	is->hot_state = IS_NONE;
	is->hot       = NULL_VARIABLE;
}

static void
ui_init(BeamformerCtx *ctx, Arena store)
{
	/* NOTE: store the ui at the base of the passed in arena and use the rest for
	 * temporary allocations within the ui. If needed we can recall this function
	 * to completely clear the ui state */
	BeamformerUI *ui = ctx->ui = alloc(&store, typeof(*ctx->ui), 1);
	ui->arena_for_frame = store;

	/* TODO: build these into the binary */
	ui->font       = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 28, 0, 0);
	ui->small_font = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 22, 0, 0);
}

static void
draw_ui(BeamformerCtx *ctx, BeamformerInput *input)
{
	BeamformerParameters *bp = &ctx->params->raw;
	BeamformerUI *ui = ctx->ui;

	end_temp_arena(ui->frame_temporary_arena);
	ui->frame_temporary_arena = begin_temp_arena(&ui->arena_for_frame);

	/* NOTE: process interactions first because the used interacted with
	 * the ui that was presented last frame */
	ui_interact(ctx, input);

	BeginDrawing();
		ClearBackground(colour_from_normalized(BG_COLOUR));

		Texture *output   = &ctx->fsctx.output.texture;

		/* TODO: this depends on the direction being rendered (x vs y) */
		v2 output_dim = {
			.x = bp->output_max_coordinate.x - bp->output_min_coordinate.x,
			.y = bp->output_max_coordinate.z - bp->output_min_coordinate.z,
		};

		v2 mouse = input->mouse;
		Rect wr = {.size = {.w = (f32)ctx->window_size.w, .h = (f32)ctx->window_size.h}};
		Rect lr = wr, rr = wr;
		lr.size.w = INFO_COLUMN_WIDTH;
		rr.size.w = wr.size.w - lr.size.w;
		rr.pos.x  = lr.pos.x  + lr.size.w;

		Rect vr = INVERTED_INFINITY_RECT;
		if (output_dim.x > 1e-6 && output_dim.y > 1e-6) {
			Stream buf = stream_alloc(&ui->arena_for_frame, 64);
			stream_append_f64(&buf, -188.8f, 10);
			stream_append_s8(&buf, s8(" mm"));
			v2 txt_s = measure_text(ui->small_font, stream_to_s8(&buf));
			buf.widx = 0;

			rr.pos.x  += 0.02 * rr.size.w;
			rr.pos.y  += 0.02 * rr.size.h;
			rr.size.w *= 0.96;
			rr.size.h *= 0.96;

			f32 tick_len = 20;
			f32 pad      = 1.2 * txt_s.x + tick_len;

			vr         = rr;
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
				/* TODO: pass this through the interaction system */
				u32 coord_idx = i == 0? 0 : 2;
				if (hover_text(mouse, tick_rect, txt_colour_t + i, 1)) {
					f32 scale[2]   = {0.5e-3, 1e-3};
					f32 size_delta = GetMouseWheelMove() * scale[i];
					/* TODO: smooth scroll this? */
					if (coord_idx== 0)
						bp->output_min_coordinate.E[coord_idx] -= size_delta;
					bp->output_max_coordinate.E[coord_idx] += size_delta;
					if (size_delta)
						ui_start_compute(ctx);
				}

				f32 mm     = bp->output_min_coordinate.E[coord_idx] * 1e3;
				f32 mm_inc = inc * output_dim.E[i] * 1e3 / vr.size.E[i];

				Color txt_colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR,
				                                                  txt_colour_t[i]));

				f32 rot[2] = {90, 0};
				for (u32 j = 0; j <= line_count; j++) {
					DrawLineEx(start_pos.rl, end_pos.rl, 3, colour_from_normalized(FG_COLOUR));
					buf.widx = 0;
					if (i == 0 && mm > 0) stream_append_byte(&buf, '+');
					stream_append_f64(&buf, mm, 10);
					stream_append_s8(&buf, s8(" mm"));
					draw_text(ui->small_font, stream_to_s8(&buf), txt_pos,
					          rot[i], txt_colour);
					start_pos.E[i] += inc;
					end_pos.E[i]   += inc;
					txt_pos.E[i]   += inc;
					mm             += mm_inc;
				}
			}
		}

		draw_settings_ui(ctx, lr, mouse);
		draw_debug_overlay(ctx, ui->arena_for_frame, lr);

		if (CheckCollisionPointRec(mouse.rl, vr.rl)) {
			InteractionState *is  = &ui->interaction;
			is->hot_state         = IS_DISPLAY;
			is->hot.store         = &ctx->fsctx.threshold;
			is->hot.type          = VT_F32;
			is->hot.f32_limits    = (v2){.y = 240};
			is->hot.flags         = V_GEN_MIPMAPS;
			is->hot.display_scale = 1;
			is->hot.scroll_scale  = 1;

			/* NOTE: check and draw Ruler */
			if (CheckCollisionPointRec(is->last_mouse_click_p.rl, vr.rl)) {
				Stream buf = arena_stream(&ui->arena_for_frame);

				Color colour = colour_from_normalized(HOVERED_COLOUR);
				DrawCircleV(is->last_mouse_click_p.rl, 3, colour);
				DrawLineEx(mouse.rl, is->last_mouse_click_p.rl, 2, colour);
				v2 pixels_to_mm = output_dim;
				pixels_to_mm.x /= vr.size.x * 1e-3;
				pixels_to_mm.y /= vr.size.y * 1e-3;

				v2 pixel_delta = sub_v2(is->last_mouse_click_p, mouse);
				v2 mm_delta    = mul_v2(pixels_to_mm, pixel_delta);

				stream_append_f64(&buf, magnitude_v2(mm_delta), 100);
				stream_append_s8(&buf, s8(" mm"));
				v2 txt_p = is->last_mouse_click_p;
				v2 txt_s = measure_text(ui->small_font, stream_to_s8(&buf));
				if (pixel_delta.y < 0) txt_p.y -= txt_s.y;
				if (pixel_delta.x < 0) txt_p.x -= txt_s.x;
				draw_text(ui->small_font, stream_to_s8(&buf), txt_p, 0, colour);
			}
		}
	EndDrawing();
}
