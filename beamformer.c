/* See LICENSE for license details. */
#include "beamformer.h"

static void
alloc_output_image(BeamformerCtx *ctx)
{
	BeamformerParameters *bp = &ctx->params->raw;
	ctx->out_data_dim.x = ORONE(bp->output_points.x);
	ctx->out_data_dim.y = ORONE(bp->output_points.y);
	ctx->out_data_dim.z = ORONE(bp->output_points.z);

	/* NOTE: allocate storage for beamformed output data;
	 * this is shared between compute and fragment shaders */
	uv4 odim    = ctx->out_data_dim;
	u32 max_dim = MAX(odim.x, MAX(odim.y, odim.z));
	/* TODO: does this actually matter or is 0 fine? */
	ctx->out_texture_unit = 0;
	ctx->out_texture_mips = _tzcnt_u32(max_dim) + 1;
	glActiveTexture(GL_TEXTURE0 + ctx->out_texture_unit);
	glDeleteTextures(1, &ctx->out_texture);
	glGenTextures(1, &ctx->out_texture);
	glBindTexture(GL_TEXTURE_3D, ctx->out_texture);
	glTexStorage3D(GL_TEXTURE_3D, ctx->out_texture_mips, GL_RG32F, odim.x, odim.y, odim.z);

	UnloadRenderTexture(ctx->fsctx.output);
	ctx->fsctx.output = LoadRenderTexture(odim.x, odim.y);
	GenTextureMipmaps(&ctx->fsctx.output.texture);
	//SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_ANISOTROPIC_8X);
	//SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_TRILINEAR);
	SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_BILINEAR);

	ctx->flags &= ~ALLOC_OUT_TEX;
}

static void
alloc_shader_storage(BeamformerCtx *ctx, Arena a)
{
	ComputeShaderCtx *cs     = &ctx->csctx;
	BeamformerParameters *bp = &ctx->params->raw;
	uv4 dec_data_dim         = bp->dec_data_dim;
	uv2 rf_raw_dim           = bp->rf_raw_dim;
	size rf_raw_size         = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);
	size rf_decoded_size     = dec_data_dim.x * dec_data_dim.y * dec_data_dim.z * sizeof(f32) * 2;
	ctx->csctx.rf_raw_dim    = rf_raw_dim;
	ctx->csctx.dec_data_dim  = dec_data_dim;

	glDeleteBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);
	glCreateBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);

	i32 storage_flags = GL_DYNAMIC_STORAGE_BIT;
	switch (ctx->gl_vendor_id) {
	case GL_VENDOR_INTEL:
	case GL_VENDOR_AMD:
		if (cs->raw_data_ssbo)
			glUnmapNamedBuffer(cs->raw_data_ssbo);
		storage_flags |= GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT;
	case GL_VENDOR_NVIDIA:
		/* NOTE: register_cuda_buffers will handle the updated ssbo */
		break;
	}

	size full_rf_buf_size = ARRAY_COUNT(cs->raw_data_fences) * rf_raw_size;
	glDeleteBuffers(1, &cs->raw_data_ssbo);
	glCreateBuffers(1, &cs->raw_data_ssbo);
	glNamedBufferStorage(cs->raw_data_ssbo, full_rf_buf_size, 0, storage_flags);

	for (u32 i = 0; i < ARRAY_COUNT(cs->rf_data_ssbos); i++)
		glNamedBufferStorage(cs->rf_data_ssbos[i], rf_decoded_size, 0, 0);

	i32 map_flags = GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_UNSYNCHRONIZED_BIT;
	switch (ctx->gl_vendor_id) {
	case GL_VENDOR_INTEL:
	case GL_VENDOR_AMD:
		cs->raw_data_arena.beg = glMapNamedBufferRange(cs->raw_data_ssbo, 0,
		                                               full_rf_buf_size, map_flags);
		break;
	case GL_VENDOR_NVIDIA:
		cs->raw_data_arena = os_alloc_arena(cs->raw_data_arena, full_rf_buf_size);
		if (g_cuda_lib_functions[CLF_REGISTER_BUFFERS] && g_cuda_lib_functions[CLF_INIT_CONFIG]) {
			register_cuda_buffers *fn = g_cuda_lib_functions[CLF_REGISTER_BUFFERS];
			fn(cs->rf_data_ssbos, ARRAY_COUNT(cs->rf_data_ssbos), cs->raw_data_ssbo);

			init_cuda_configuration *init_fn = g_cuda_lib_functions[CLF_INIT_CONFIG];
			init_fn(bp->rf_raw_dim.E, bp->dec_data_dim.E, bp->channel_mapping,
			        bp->channel_offset > 0);
		}
		break;
	}

	/* NOTE: store hadamard in GPU once; it won't change for a particular imaging session */
	cs->hadamard_dim       = (uv2){.x = dec_data_dim.z, .y = dec_data_dim.z};
	size hadamard_elements = dec_data_dim.z * dec_data_dim.z;
	i32  *hadamard         = alloc(&a, i32, hadamard_elements);
	fill_hadamard(hadamard, dec_data_dim.z);
	glDeleteBuffers(1, &cs->hadamard_ssbo);
	glCreateBuffers(1, &cs->hadamard_ssbo);
	glNamedBufferStorage(cs->hadamard_ssbo, hadamard_elements * sizeof(i32), hadamard, 0);

	ctx->flags &= ~ALLOC_SSBOS;
}

static void
do_compute_shader(BeamformerCtx *ctx, enum compute_shaders shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	uv2  rf_raw_dim         = ctx->params->raw.rf_raw_dim;
	size rf_raw_size        = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);

	glBeginQuery(GL_TIME_ELAPSED, csctx->timer_ids[csctx->timer_index][shader]);

	glUseProgram(csctx->programs[shader]);

	glBindBufferBase(GL_UNIFORM_BUFFER, 0, csctx->shared_ubo);

	u32 output_ssbo_idx = !csctx->last_output_ssbo_index;
	u32 input_ssbo_idx  = csctx->last_output_ssbo_index;
	switch (shader) {
	case CS_HADAMARD:
		glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, csctx->raw_data_ssbo,
		                  csctx->raw_data_index * rf_raw_size, rf_raw_size);

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, csctx->hadamard_ssbo);
		glDispatchCompute(ORONE(csctx->dec_data_dim.x / 32),
		                  ORONE(csctx->dec_data_dim.y / 32),
		                  ORONE(csctx->dec_data_dim.z));
		csctx->raw_data_fences[csctx->raw_data_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_CUDA_DECODE_AND_DEMOD:
		if (g_cuda_lib_functions[CLF_DECODE_AND_DEMOD]) {
			decode_and_hilbert*fn = g_cuda_lib_functions[CLF_DECODE_AND_DEMOD];

			fn(csctx->raw_data_index * rf_raw_size, output_ssbo_idx);
			csctx->raw_data_fences[csctx->raw_data_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		}
		break;
	case CS_LPF:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glDispatchCompute(ORONE(csctx->dec_data_dim.x / 32),
		                  ORONE(csctx->dec_data_dim.y / 32),
		                  ORONE(csctx->dec_data_dim.z));
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_MIN_MAX:
		glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_FALSE, 0,
		                   GL_WRITE_ONLY, GL_RG32F);
		glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);
		for (u32 i = 1; i < ctx->out_texture_mips; i++) {
			u32 otu = ctx->out_texture_unit;
			glBindImageTexture(otu + 1, ctx->out_texture, i - 1,
			                   GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
			glBindImageTexture(otu + 2, ctx->out_texture, i,
			                   GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32F);
			glUniform1i(csctx->out_data_tex_id, otu + 1);
			glUniform1i(csctx->mip_view_tex_id, otu + 2);
			glUniform1i(csctx->mips_level_id, i);

			u32 width  = ctx->out_data_dim.x >> i;
			u32 height = ctx->out_data_dim.y >> i;
			u32 depth  = ctx->out_data_dim.z >> i;
			glDispatchCompute(ORONE(width / 32), ORONE(height / 32), ORONE(depth));
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		}
		break;
	case CS_UFORCES:
		glActiveTexture(GL_TEXTURE0 + ctx->out_texture_unit);
		glBindTexture(GL_TEXTURE_3D, ctx->out_texture);
		glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_FALSE, 0,
		                   GL_WRITE_ONLY, GL_RG32F);
		glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glDispatchCompute(ORONE(ctx->out_data_dim.x / 32),
		                  ORONE(ctx->out_data_dim.y / 32),
		                  ORONE(ctx->out_data_dim.z));
		break;
	default: ASSERT(0);
	}

	glEndQuery(GL_TIME_ELAPSED);
}

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
		[CS_CUDA_DECODE_AND_DEMOD] = s8("CUDA Decoding:"),
		[CS_HADAMARD]              = s8("Decoding:"),
		[CS_HERCULES]              = s8("HERCULES:"),
		[CS_LPF]                   = s8("LPF:"),
		[CS_MIN_MAX]               = s8("Min/Max:"),
		[CS_UFORCES]               = s8("UFORCES:"),
	};

	ComputeShaderCtx *cs = &ctx->csctx;

	s8 txt_buf = s8alloc(&arena, 64);
	v2 pos = {.x = 20, .y = ws.h - 10};

	u32 stages = ctx->params->compute_stages_count;
	for (u32 i = 0; i < stages; i++) {
		u32 index  = ctx->params->compute_stages[i];
		v2 txt_fs  = measure_text(ctx->font, labels[index]);
		pos.y     -= txt_fs.y;
		draw_text(ctx->font, labels[index], pos, 0, colour_from_normalized(FG_COLOUR));

		s8 tmp = txt_buf;
		tmp.len = snprintf((char *)txt_buf.data, txt_buf.len, "%0.02e [s]",
		                   cs->last_frame_time[index]);
		txt_fs = measure_text(ctx->font, tmp);
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
check_compute_timers(ComputeShaderCtx *cs)
{
	u32 last_idx = (cs->timer_index - 1) % ARRAY_COUNT(cs->timer_fences);
	if (!cs->timer_fences[last_idx])
		return;

	i32 status = glClientWaitSync(cs->timer_fences[last_idx], 0, 0);
	if (status == GL_TIMEOUT_EXPIRED || status == GL_WAIT_FAILED)
		return;

	for (u32 i = 0; i < ARRAY_COUNT(cs->last_frame_time); i++) {
		u64 ns = 0;
		glGetQueryObjectui64v(cs->timer_ids[last_idx][i], GL_QUERY_RESULT, &ns);
		cs->last_frame_time[i] = (f32)ns / 1e9;
	}
}

DEBUG_EXPORT void
do_beamformer(BeamformerCtx *ctx, Arena arena)
{
	ctx->dt = GetFrameTime();

	if (IsWindowResized()) {
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = GetScreenWidth();
	}

	/* NOTE: Store the compute time for the last frame. */
	check_compute_timers(&ctx->csctx);

	BeamformerParameters *bp = &ctx->params->raw;
	/* NOTE: Check for and Load RF Data into GPU */
	if (os_poll_pipe(ctx->data_pipe)) {
		ComputeShaderCtx *cs = &ctx->csctx;
		if (!uv4_equal(cs->dec_data_dim, bp->dec_data_dim) || ctx->flags & ALLOC_SSBOS)
			alloc_shader_storage(ctx, arena);

		if (!uv4_equal(ctx->out_data_dim, bp->output_points) || ctx->flags & ALLOC_OUT_TEX)
			alloc_output_image(ctx);

		cs->raw_data_index = (cs->raw_data_index + 1) % ARRAY_COUNT(cs->raw_data_fences);
		i32 raw_index = ctx->csctx.raw_data_index;
		/* NOTE: if this times out it means the command queue is more than 3 frames behind.
		 * In that case we need to re-evaluate the buffer size */
		if (ctx->csctx.raw_data_fences[raw_index]) {
			i32 result = glClientWaitSync(cs->raw_data_fences[raw_index], 0, 10000);
			if (result == GL_TIMEOUT_EXPIRED) {
				//ASSERT(0);
			}
			glDeleteSync(cs->raw_data_fences[raw_index]);
			cs->raw_data_fences[raw_index] = NULL;
		}

		uv2  rf_raw_dim   = cs->rf_raw_dim;
		size rf_raw_size  = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);

		void *rf_data_buf = cs->raw_data_arena.beg + raw_index * rf_raw_size;
		size rlen         = os_read_pipe_data(ctx->data_pipe, rf_data_buf, rf_raw_size);
		switch (ctx->gl_vendor_id) {
		case GL_VENDOR_INTEL:
			/* TODO: intel complains about this buffer being busy even with
			 * MAP_UNSYNCHRONIZED_BIT */
		case GL_VENDOR_AMD:
			break;
		case GL_VENDOR_NVIDIA:
			glNamedBufferSubData(cs->raw_data_ssbo, raw_index * rf_raw_size,
			                     rf_raw_size, rf_data_buf);
		}
		if (rlen == rf_raw_size) ctx->flags |= DO_COMPUTE;
		else                     ctx->partial_transfer_count++;
	}

	if (ctx->flags & DO_COMPUTE) {
		if (ctx->params->upload) {
			glNamedBufferSubData(ctx->csctx.shared_ubo, 0, sizeof(*bp), bp);
			ctx->params->upload = 0;
		}
		u32 stages = ctx->params->compute_stages_count;
		for (u32 i = 0; i < stages; i++) {
			do_compute_shader(ctx, ctx->params->compute_stages[i]);
		}
		ctx->flags &= ~DO_COMPUTE;
		ctx->flags |= GEN_MIPMAPS;

		u32 tidx = ctx->csctx.timer_index;
		glDeleteSync(ctx->csctx.timer_fences[tidx]);
		ctx->csctx.timer_fences[tidx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		ctx->csctx.timer_index = (tidx + 1) % ARRAY_COUNT(ctx->csctx.timer_fences);
	}

	/* NOTE: draw output image texture using render fragment shader */
	BeginTextureMode(ctx->fsctx.output);
		ClearBackground(PINK);
		BeginShaderMode(ctx->fsctx.shader);
			FragmentShaderCtx *fs = &ctx->fsctx;
			glUseProgram(fs->shader.id);
			glUniform1i(fs->out_data_tex_id, ctx->out_texture_unit);
			glUniform1f(fs->db_cutoff_id, fs->db);
			DrawTexture(fs->output.texture, 0, 0, WHITE);
		EndShaderMode();
	EndTextureMode();

	/* NOTE: regenerate mipmaps only when the output has actually changed */
	if (ctx->flags & GEN_MIPMAPS) {
		/* NOTE: shut up raylib's reporting on mipmap gen */
		SetTraceLogLevel(LOG_NONE);
		GenTextureMipmaps(&ctx->fsctx.output.texture);
		SetTraceLogLevel(LOG_INFO);
		ctx->flags &= ~GEN_MIPMAPS;
	}

	/* NOTE: Draw UI */
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

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
}
