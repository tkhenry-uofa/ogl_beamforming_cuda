/* See LICENSE for license details. */
#define BG_COLOUR              (v4){.r = 0.15, .g = 0.12, .b = 0.13, .a = 1.0}
#define FG_COLOUR              (v4){.r = 0.92, .g = 0.88, .b = 0.78, .a = 1.0}
#define FOCUSED_COLOUR         (v4){.r = 0.86, .g = 0.28, .b = 0.21, .a = 1.0}
#define HOVERED_COLOUR         (v4){.r = 0.11, .g = 0.50, .b = 0.59, .a = 1.0}
#define RULER_COLOUR           (v4){.r = 1.00, .g = 0.70, .b = 0.00, .a = 1.0}

#define NORMALIZED_FG_COLOUR   colour_from_normalized(FG_COLOUR)

#define HOVER_SPEED            5.0f

#define RULER_TEXT_PAD         10.0f
#define RULER_TICK_LENGTH      20.0f

#define UI_SPLIT_HANDLE_THICK  8.0f
#define UI_REGION_PAD          16.0f

typedef struct {
	u8   buf[64];
	i32  buf_len;
	i32  cursor;
	f32  cursor_blink_t;
	f32  cursor_blink_scale;
	Rect rect, hot_rect;
	Font *font, *hot_font;
} InputState;

typedef enum {
	IT_NONE,
	IT_NOP,
	IT_SET,
	IT_DRAG,
	IT_SCROLL,
	IT_TEXT,

	IT_DISPLAY,
	IT_SCALE_BAR,
} InteractionType;

enum ruler_state {
	RS_NONE,
	RS_START,
	RS_HOLD,
};

typedef struct v2_sll {
	struct v2_sll *next;
	v2             v;
} v2_sll;

typedef struct Variable Variable;

typedef enum {
	RSD_VERTICAL,
	RSD_HORIZONTAL,
} RegionSplitDirection;

typedef struct {
	Variable *left;
	Variable *right;
	f32       fraction;
	RegionSplitDirection direction;
} RegionSplit;

/* TODO(rnp): this should be refactored to not need a BeamformerCtx */
typedef struct {
	BeamformerCtx *ctx;
	void          *stats;
} ComputeStatsView;

typedef enum {
	VT_NULL,
	VT_B32,
	VT_F32,
	VT_I32,
	VT_GROUP,
	VT_BEAMFORMER_VARIABLE,
	VT_BEAMFORMER_FRAME_VIEW,
	VT_COMPUTE_STATS_VIEW,
	VT_COMPUTE_LATEST_STATS_VIEW,
	VT_SCALE_BAR,
	VT_UI_REGION_SPLIT,
} VariableType;

typedef enum {
	VG_LIST,
	/* NOTE(rnp): special groups for vectors with components
	 * stored in separate memory locations */
	VG_V2,
	VG_V4,
} VariableGroupType;

typedef struct {
	Variable *first;
	Variable *last;
	b32       expanded;
	f32       max_name_width;
	VariableGroupType type;
} VariableGroup;

struct Variable {
	s8 name;
	union {
		void            *generic;
		VariableGroup    group;
		RegionSplit      region_split;
		ComputeStatsView compute_stats_view;
		b32              b32;
		i32              i32;
		f32              f32;
	} u;
	Variable *next;
	Variable *parent;
	u32       flags;
	VariableType type;

	f32 hover_t;
	f32 name_width;
};

enum variable_flags {
	V_INPUT          = 1 << 0,
	V_TEXT           = 1 << 1,
	V_CAUSES_COMPUTE = 1 << 29,
	V_GEN_MIPMAPS    = 1 << 30,
};

typedef struct {
	Variable base;
	s8       suffix;
	/* TODO(rnp): think of something better than this */
	union {
		struct {s8 *names; u32 count;} name_table;
		struct {
			f32 display_scale;
			f32 scroll_scale;
			v2  limits;
		} params;
	};
	VariableType subtype;
} BeamformerVariable;

typedef enum {
	SB_LATERAL,
	SB_AXIAL,
} ScaleBarDirection;

typedef struct {
	f32    *min_value, *max_value;
	v2_sll *savepoint_stack;
	v2      zoom_starting_point;
	v2      screen_offset;
	v2      screen_space_to_value;
	v2      limits;
	v2      scroll_scale;
	f32     hover_t;
} ScaleBar;

typedef enum {
	FVT_INDEXED,
	FVT_LATEST,
	FVT_COPY,
} BeamformerFrameViewType;

typedef struct {
	void *store;
	ScaleBar lateral_scale_bar;
	ScaleBar axial_scale_bar;
	Variable menu;
	/* TODO(rnp): hack, this needs to be removed to support multiple views */
	FragmentShaderCtx *ctx;
	BeamformerFrameViewType type;
} BeamformerFrameView;

typedef struct {
	Variable *hot;
	Variable *active;
	InteractionType hot_type;
	InteractionType type;
} InteractionState;

typedef struct {
	Arena arena;

	Font font;
	Font small_font;

	BeamformerVariable *scratch_variable;
	BeamformerVariable scratch_variables[2];

	InteractionState interaction;
	InputState       text_input_state;

	Variable *regions;

	v2_sll *scale_bar_savepoint_freelist;

	BeamformFrame      *latest_frame;
	ComputeShaderStats *latest_compute_stats;

	v2  ruler_start_p;
	v2  ruler_stop_p;
	u32 ruler_state;

	f32 progress_display_t;
	f32 progress_display_t_velocity;

	BeamformerUIParameters params;
	b32                    flush_params;

	iptr                   last_displayed_frame;
} BeamformerUI;

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
stream_append_variable_base(Stream *s, VariableType type, void *var, void *scale)
{
	switch (type) {
	case VT_B32: {
		s8 *text = var;
		stream_append_s8(s, text[*(b32 *)scale != 0]);
	} break;
	case VT_F32: {
		f32 val = *(f32 *)var * *(f32 *)scale;
		stream_append_f64(s, val, 100);
	} break;
	default: INVALID_CODE_PATH;
	}
}

static void
stream_append_variable(Stream *s, Variable *var)
{
	switch (var->type) {
	case VT_GROUP: stream_append_s8(s, var->name); break;
	case VT_BEAMFORMER_VARIABLE: {
		BeamformerVariable *bv = (BeamformerVariable *)var;
		switch (bv->subtype) {
		case VT_B32: {
			stream_append_variable_base(s, VT_B32, bv->name_table.names, bv->base.u.generic);
		} break;
		case VT_F32: {
			stream_append_variable_base(s, VT_F32, bv->base.u.generic, &bv->params.display_scale);
		} break;
		default: INVALID_CODE_PATH;
		}
	} break;
	case VT_B32: {
		s8 texts[] = {s8("False"), s8("True")};
		stream_append_variable_base(s, VT_B32, texts, &var->u.b32);
	} break;
	case VT_F32: {
		f32 scale = 1;
		stream_append_variable_base(s, VT_F32, &var->u.f32, &scale);
	} break;
	default: INVALID_CODE_PATH;
	}
}


static Variable *
add_variable(Variable *group, Arena *arena, s8 name, u32 flags, VariableType type, Font font)
{
	Variable *result;
	if (type == VT_BEAMFORMER_VARIABLE) result = (Variable *)push_struct(arena, BeamformerVariable);
	else                                result = push_struct(arena, Variable);
	result->flags      = flags;
	result->type       = type;
	result->name       = name;
	result->parent     = group;
	result->name_width = measure_text(font, name).x;

	if (group && group->type == VT_GROUP) {
		if (group->u.group.last) group->u.group.last = group->u.group.last->next = result;
		else                     group->u.group.last = group->u.group.first      = result;

		group->u.group.max_name_width = MAX(group->u.group.max_name_width, result->name_width);
	}

	return result;
}

static Variable *
add_variable_group(Variable *group, Arena *arena, s8 name, VariableGroupType type, Font font)
{
	Variable *result     = add_variable(group, arena, name, V_INPUT, VT_GROUP, font);
	result->u.group.type = type;
	return result;
}

static Variable *
end_variable_group(Variable *group)
{
	ASSERT(group->type == VT_GROUP);
	return group->parent;
}

static Variable *
add_ui_split(Variable *parent, Arena *arena, s8 name, f32 fraction,
             RegionSplitDirection direction, Font font)
{
	Variable *result = add_variable(parent, arena, name, 0, VT_UI_REGION_SPLIT, font);
	result->u.region_split.direction = direction;
	result->u.region_split.fraction  = fraction;
	return result;
}

static void
add_beamformer_variable_f32(Variable *group, Arena *arena, s8 name, s8 suffix, f32 *store,
                            v2 limits, f32 display_scale, f32 scroll_scale, u32 flags, Font font)
{
	BeamformerVariable *bv   = (BeamformerVariable *)add_variable(group, arena, name, flags,
	                                                              VT_BEAMFORMER_VARIABLE, font);
	bv->suffix               = suffix;
	bv->base.u.generic       = store;
	bv->subtype              = VT_F32;
	bv->params.display_scale = display_scale;
	bv->params.scroll_scale  = scroll_scale;
	bv->params.limits        = limits;
}

static void
add_beamformer_variable_b32(Variable *group, Arena *arena, s8 name, s8 false_text, s8 true_text,
                            b32 *store, u32 flags, Font font)
{
	BeamformerVariable *bv  = (BeamformerVariable *)add_variable(group, arena, name, flags,
	                                                             VT_BEAMFORMER_VARIABLE, font);
	bv->base.u.generic      = store;
	bv->subtype             = VT_B32;
	bv->name_table.names    = alloc(arena, s8, 2);
	bv->name_table.count    = 2;
	bv->name_table.names[0] = false_text;
	bv->name_table.names[1] = true_text;
}

static Variable *
add_beamformer_parameters_view(Variable *parent, BeamformerCtx *ctx)
{
	BeamformerUI *ui           = ctx->ui;
	BeamformerUIParameters *bp = &ui->params;

	v2 v2_inf = {.x = -F32_INFINITY, .y = F32_INFINITY};

	Variable *result = add_variable_group(parent, &ui->arena, s8("Parameters List"), VG_LIST, ui->font);

	add_beamformer_variable_f32(result, &ui->arena, s8("Sampling Frequency:"), s8("[MHz]"),
	                            &bp->sampling_frequency, (v2){0}, 1e-6, 0, 0, ui->font);

	add_beamformer_variable_f32(result, &ui->arena, s8("Center Frequency:"), s8("[MHz]"),
	                            &bp->center_frequency, (v2){.y = 100e-6}, 1e-6, 1e5,
	                            V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);

	add_beamformer_variable_f32(result, &ui->arena, s8("Speed of Sound:"), s8("[m/s]"),
	                            &bp->speed_of_sound, (v2){.y = 1e6}, 1, 10,
	                            V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);

	result = add_variable_group(result, &ui->arena, s8("Lateral Extent:"), VG_V2, ui->font);
	{
		add_beamformer_variable_f32(result, &ui->arena, s8("Min:"), s8("[mm]"),
		                            &bp->output_min_coordinate.x, v2_inf, 1e3, 0.5e-3,
		                            V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);

		add_beamformer_variable_f32(result, &ui->arena, s8("Max:"), s8("[mm]"),
		                            &bp->output_max_coordinate.x, v2_inf, 1e3, 0.5e-3,
		                            V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);
	}
	result = end_variable_group(result);

	result = add_variable_group(result, &ui->arena, s8("Axial Extent:"), VG_V2, ui->font);
	{
		add_beamformer_variable_f32(result, &ui->arena, s8("Min:"), s8("[mm]"),
		                            &bp->output_min_coordinate.z, v2_inf, 1e3, 0.5e-3,
		                            V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);

		add_beamformer_variable_f32(result, &ui->arena, s8("Max:"), s8("[mm]"),
		                            &bp->output_max_coordinate.z, v2_inf, 1e3, 0.5e-3,
		                            V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);
	}
	result = end_variable_group(result);

	add_beamformer_variable_f32(result, &ui->arena, s8("Off Axis Position:"), s8("[mm]"),
	                            &bp->off_axis_pos, (v2){.x = -1e3, .y = 1e3}, 1e3,
	                            0.5e-3, V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);

	add_beamformer_variable_b32(result, &ui->arena, s8("Beamform Plane:"), s8("XZ"), s8("YZ"),
	                            (b32 *)&bp->beamform_plane, V_INPUT|V_CAUSES_COMPUTE, ui->font);

	add_beamformer_variable_f32(result, &ui->arena, s8("F#:"), s8(""), &bp->f_number,
	                            (v2){.y = 1e3}, 1, 0.1, V_INPUT|V_TEXT|V_CAUSES_COMPUTE, ui->font);

	add_beamformer_variable_f32(result, &ui->arena, s8("Dynamic Range:"), s8("[dB]"),
	                            &ctx->fsctx.db, (v2){.x = -120}, 1, 1,
	                            V_INPUT|V_TEXT|V_GEN_MIPMAPS, ui->font);

	add_beamformer_variable_f32(result, &ui->arena, s8("Threshold:"), s8(""),
	                            &ctx->fsctx.threshold, (v2){.y = 240}, 1, 1,
	                            V_INPUT|V_TEXT|V_GEN_MIPMAPS, ui->font);

	return result;
}

static Variable *
add_beamformer_frame_view(Variable *parent, Arena *arena, BeamformerFrameViewType type, Font font)
{
	Variable *result = add_variable(parent, arena, s8(""), 0, VT_BEAMFORMER_FRAME_VIEW, font);
	BeamformerFrameView *bv = result->u.generic = push_struct(arena, typeof(*bv));
	bv->type = type;
	return result;
}

static BeamformFrame *
beamformer_frame_view_frame(BeamformerFrameView *bv)
{
	BeamformFrame *result;
	switch (bv->type) {
	case FVT_LATEST: result = *(BeamformFrame **)bv->store; break;
	default:         result = bv->store;                    break;
	}
	return result;
}

static Color
colour_from_normalized(v4 rgba)
{
	return (Color){.r = rgba.r * 255.0f, .g = rgba.g * 255.0f,
	               .b = rgba.b * 255.0f, .a = rgba.a * 255.0f};
}

static Color
fade(Color a, f32 visibility)
{
	a.a = (u8)((f32)a.a * visibility);
	return a;
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

static s8
push_das_shader_id(Stream *s, DASShaderID shader, u32 transmit_count)
{
	switch (shader) {
	#define X(type, id, pretty) case id: stream_append_s8(s, s8(pretty)); break;
	DAS_TYPES
	#undef X
	default: break;
	}

	switch (shader) {
	case DAS_UFORCES:
	case DAS_RCA_VLS:
	case DAS_RCA_TPW:
		stream_append_byte(s, '-');
		stream_append_u64(s, transmit_count);
	default: break;
	}

	return stream_to_s8(s);
}

static v2
draw_text(Font font, s8 text, v2 pos, Color colour)
{
	v2 off = pos;
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
	v2 result = {.x = off.x - pos.x, .y = font.baseSize};
	return result;
}

static v2
draw_text_limited(Font font, s8 text, v2 pos, Color colour, f32 space, f32 text_width)
{
	v2 result = {.y = font.baseSize};
	if (text_width < space) {
		result = draw_text(font, text, pos, colour);
	} else {
		f32 elipsis_width = measure_text(font, s8("...")).x;
		if (elipsis_width < space) {
			/* TODO(rnp): there must be a better way */
			while (text.len) {
				text.len--;
				f32 width = measure_text(font, text).x;
				if (width + elipsis_width < space)
					break;
			}
			result.x += draw_text(font, text, pos, colour).x;
			pos.x    += result.x;
			result.x += draw_text(font, s8("..."), pos, colour).x;
		}
	}
	return result;
}

/* NOTE(rnp): expensive but of the available options in raylib this gives the best results */
static v2
draw_outlined_text(Font font, s8 text, v2 pos, f32 outline_width, Color colour, Color outline)
{
	draw_text(font, text, sub_v2(pos, (v2){.x =  outline_width, .y =  outline_width}), outline);
	draw_text(font, text, sub_v2(pos, (v2){.x =  outline_width, .y = -outline_width}), outline);
	draw_text(font, text, sub_v2(pos, (v2){.x = -outline_width, .y =  outline_width}), outline);
	draw_text(font, text, sub_v2(pos, (v2){.x = -outline_width, .y = -outline_width}), outline);

	v2 result = draw_text(font, text, pos, colour);

	return result;
}

static v2
draw_text_r(Font font, s8 text, v2 pos, f32 rotation, Color colour)
{
	rlPushMatrix();

	rlTranslatef(pos.x, pos.y, 0);
	rlRotatef(rotation, 0, 0, 1);

	v2 result = draw_text(font, text, (v2){0}, colour);

	rlPopMatrix();
	return result;
}

static Rect
extend_rect_centered(Rect r, v2 delta)
{
	r.size.w += delta.x;
	r.size.h += delta.y;
	r.pos.x  -= delta.x / 2;
	r.pos.y  -= delta.y / 2;
	return r;
}

static Rect
shrink_rect_centered(Rect r, v2 delta)
{
	r.size.w -= delta.x;
	r.size.h -= delta.y;
	r.pos.x  += delta.x / 2;
	r.pos.y  += delta.y / 2;
	return r;
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
hover_rect(v2 mouse, Rect rect, f32 *hover_t)
{
	b32 hovering = CheckCollisionPointRec(mouse.rl, rect.rl);
	if (hovering) *hover_t += HOVER_SPEED * dt_for_frame;
	else          *hover_t -= HOVER_SPEED * dt_for_frame;
	*hover_t = CLAMP01(*hover_t);
	return hovering;
}

/* TODO(rnp): once this has more callers decide if it would be better for this to take
 * an orientation rather than force CCW/right-handed */
static void
draw_ruler(BeamformerUI *ui, Stream *buf, v2 start_point, v2 end_point,
           f32 start_value, f32 end_value, f32 *markers, u32 marker_count,
           u32 segments, s8 suffix, Color ruler_colour, Color txt_colour)
{
	b32 draw_plus = SIGN(start_value) != SIGN(end_value);

	end_point    = sub_v2(end_point, start_point);
	f32 rotation = atan2_f32(end_point.y, end_point.x) * 180 / PI;

	rlPushMatrix();
	rlTranslatef(start_point.x, start_point.y, 0);
	rlRotatef(rotation, 0, 0, 1);

	f32 inc       = magnitude_v2(end_point) / segments;
	f32 value_inc = (end_value - start_value) / segments;
	f32 value     = start_value;

	v2 sp = {0}, ep = {.y = RULER_TICK_LENGTH};
	v2 tp = {.x = ui->small_font.baseSize / 2, .y = ep.y + RULER_TEXT_PAD};
	for (u32 j = 0; j <= segments; j++) {
		DrawLineEx(sp.rl, ep.rl, 3, ruler_colour);

		buf->widx = 0;
		if (draw_plus && value > 0) stream_append_byte(buf, '+');
		stream_append_f64(buf, value, 10);
		stream_append_s8(buf, suffix);
		draw_text_r(ui->small_font, stream_to_s8(buf), tp, 90, txt_colour);

		value += value_inc;
		sp.x  += inc;
		ep.x  += inc;
		tp.x  += inc;
	}

	ep.y += RULER_TICK_LENGTH;
	for (u32 i = 0; i < marker_count; i++) {
		if (markers[i] < F32_INFINITY) {
			ep.x  = sp.x = markers[i];
			DrawLineEx(sp.rl, ep.rl, 3, colour_from_normalized(RULER_COLOUR));
			DrawCircleV(ep.rl, 3, colour_from_normalized(RULER_COLOUR));
		}
	}

	rlPopMatrix();
}

static void
do_scale_bar(BeamformerUI *ui, Stream *buf, ScaleBar *sb, ScaleBarDirection direction,
             v2 mouse, Rect draw_rect, f32 start_value, f32 end_value, s8 suffix)
{
	InteractionState *is = &ui->interaction;

	v2 txt_s = measure_text(ui->small_font, s8("-288.8 mm"));

	Rect tick_rect = draw_rect;
	v2   start_pos = tick_rect.pos;
	v2   end_pos   = tick_rect.pos;
	v2   relative_mouse = sub_v2(mouse, tick_rect.pos);

	f32  markers[2];
	u32  marker_count = 1;

	u32  tick_count;
	if (direction == SB_AXIAL) {
		tick_rect.size.x  = RULER_TEXT_PAD + RULER_TICK_LENGTH + txt_s.x;
		tick_count        = tick_rect.size.y / (1.5 * ui->small_font.baseSize);
		start_pos.y      += tick_rect.size.y;
		markers[0]        = tick_rect.size.y - sb->zoom_starting_point.y;
		markers[1]        = tick_rect.size.y - relative_mouse.y;
		sb->screen_offset = (v2){.y = tick_rect.pos.y};
		sb->screen_space_to_value = (v2){.y = (*sb->max_value - *sb->min_value) / tick_rect.size.y};
	} else {
		tick_rect.size.y  = RULER_TEXT_PAD + RULER_TICK_LENGTH + txt_s.x;
		tick_count        = tick_rect.size.x / (1.5 * ui->small_font.baseSize);
		end_pos.x        += tick_rect.size.x;
		markers[0]        = sb->zoom_starting_point.x;
		markers[1]        = relative_mouse.x;
		/* TODO(rnp): screen space to value space transformation helper */
		sb->screen_offset = (v2){.x = tick_rect.pos.x};
		sb->screen_space_to_value = (v2){.x = (*sb->max_value - *sb->min_value) / tick_rect.size.x};
	}

	if (hover_rect(mouse, tick_rect, &sb->hover_t)) {
		Variable *var  = zero_struct(&ui->scratch_variable->base);
		var->u.generic = sb;
		var->type      = VT_SCALE_BAR;
		is->hot_type   = IT_SCALE_BAR;
		is->hot        = var;

		marker_count  = 2;
	}

	draw_ruler(ui, buf, start_pos, end_pos, start_value, end_value, markers, marker_count,
	           tick_count, suffix, colour_from_normalized(FG_COLOUR),
	           colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, sb->hover_t)));
}

static void
draw_beamformer_frame_view(BeamformerUI *ui, Arena a, BeamformerFrameView *view,
                           Rect display_rect, v2 mouse)
{
	BeamformerUIParameters *bp = &ui->params;
	InteractionState *is       = &ui->interaction;
	BeamformFrame *frame       = beamformer_frame_view_frame(view);

	Stream buf      = arena_stream(&a);
	Texture *output = &view->ctx->output.texture;

	v2 txt_s = measure_text(ui->small_font, s8("-288.8 mm"));

	display_rect = shrink_rect_centered(display_rect, (v2){.x = 2 * UI_REGION_PAD, .y = UI_REGION_PAD});

	f32 pad    = 1.2 * txt_s.x + RULER_TICK_LENGTH;
	Rect vr    = display_rect;
	vr.pos.x  += 0.5 * ui->small_font.baseSize;
	vr.pos.y  += 0.5 * ui->small_font.baseSize;
	vr.size.h  = MAX(0, display_rect.size.h - pad);
	vr.size.w  = MAX(0, display_rect.size.w - pad);

	/* TODO(rnp): make this depend on the requested draw orientation (x-z or y-z or x-y) */
	v2 output_dim = {
		.x = frame->max_coordinate.x - frame->min_coordinate.x,
		.y = frame->max_coordinate.z - frame->min_coordinate.z,
	};
	v2 requested_dim = {
		.x = bp->output_max_coordinate.x - bp->output_min_coordinate.x,
		.y = bp->output_max_coordinate.z - bp->output_min_coordinate.z,
	};

	f32 aspect = requested_dim.h / requested_dim.w;
	if (display_rect.size.h < (vr.size.w * aspect) + pad) {
		vr.size.w = vr.size.h / aspect;
	} else {
		vr.size.h = vr.size.w * aspect;
	}
	vr.pos.x += (display_rect.size.w - (vr.size.w + pad)) / 2;
	vr.pos.y += (display_rect.size.h - (vr.size.h + pad)) / 2;

	v2 pixels_per_meter = {
		.w = (f32)output->width  / output_dim.w,
		.h = (f32)output->height / output_dim.h,
	};

	v2 texture_points  = mul_v2(pixels_per_meter, requested_dim);
	/* TODO(rnp): this also depends on x-y, y-z, x-z */
	v2 texture_start   = {
		.x = pixels_per_meter.x * 0.5 * (output_dim.x - requested_dim.x),
		.y = pixels_per_meter.y * (frame->max_coordinate.z - bp->output_max_coordinate.z),
	};

	Rectangle  tex_r  = {texture_start.x, texture_start.y, texture_points.x, -texture_points.y};
	NPatchInfo tex_np = { tex_r, 0, 0, 0, 0, NPATCH_NINE_PATCH };
	DrawTextureNPatch(*output, tex_np, vr.rl, (Vector2){0}, 0, WHITE);

	v2 start_pos  = vr.pos;
	start_pos.y  += vr.size.y;

	if (vr.size.w > 0) {
		do_scale_bar(ui, &buf, &view->lateral_scale_bar, SB_LATERAL, mouse,
		             (Rect){.pos = start_pos, .size = vr.size},
		             bp->output_min_coordinate.x * 1e3,
		             bp->output_max_coordinate.x * 1e3, s8(" mm"));
	}

	start_pos    = vr.pos;
	start_pos.x += vr.size.x;

	if (vr.size.h > 0) {
		do_scale_bar(ui, &buf, &view->axial_scale_bar, SB_AXIAL, mouse,
		             (Rect){.pos = start_pos, .size = vr.size},
		             bp->output_max_coordinate.z * 1e3,
		             bp->output_min_coordinate.z * 1e3, s8(" mm"));
	}

	v2 pixels_to_mm = output_dim;
	pixels_to_mm.x /= vr.size.x * 1e-3;
	pixels_to_mm.y /= vr.size.y * 1e-3;

	if (CheckCollisionPointRec(mouse.rl, vr.rl)) {
		BeamformerVariable *bv   = zero_struct(ui->scratch_variable);
		bv->base.type            = VT_BEAMFORMER_VARIABLE;
		bv->base.u.generic       = &view->ctx->threshold;
		bv->base.flags           = V_GEN_MIPMAPS;
		bv->subtype              = VT_F32;
		bv->params.limits        = (v2){.y = 240};
		bv->params.display_scale = 1;
		bv->params.scroll_scale  = 1;

		is->hot_type = IT_DISPLAY;
		is->hot      = (Variable *)bv;


		v2 relative_mouse = sub_v2(mouse, vr.pos);
		v2 mm = mul_v2(relative_mouse, pixels_to_mm);
		mm.x += 1e3 * bp->output_min_coordinate.x;
		mm.y += 1e3 * bp->output_min_coordinate.z;

		buf.widx = 0;
		stream_append_v2(&buf, mm);
		v2 txt_s = measure_text(ui->small_font, stream_to_s8(&buf));
		if (vr.size.w + vr.pos.x - txt_s.w - 4 > display_rect.pos.x && txt_s.h < display_rect.size.h - 4) {
			v2 txt_p = {
				.x = vr.pos.x + vr.size.w - txt_s.w - 4,
				.y = vr.pos.y + vr.size.h - txt_s.h - 4,
			};
			draw_outlined_text(ui->small_font, stream_to_s8(&buf), txt_p, 1,
			                   colour_from_normalized(RULER_COLOUR), BLACK);
		}
	}

	{
		buf.widx  = 0;
		s8 shader = push_das_shader_id(&buf, frame->das_shader_id, frame->compound_count);
		v2 txt_s  = measure_text(ui->font, shader);
		/* TODO(rnp): we want this 16 to be proportional to vr size */
		if (vr.size.w + vr.pos.x - txt_s.w - 16 > display_rect.pos.x && txt_s.h < display_rect.size.h - 4) {
			v2 txt_p  = {
				.x = vr.pos.x + vr.size.w - txt_s.w - 16,
				.y = vr.pos.y + 4,
			};
			draw_outlined_text(ui->font, shader, txt_p, 1,
			                   colour_from_normalized(RULER_COLOUR), BLACK);
		}
	}

	/* TODO(rnp): store converted ruler points instead of screen points */
	if (ui->ruler_state != RS_NONE && CheckCollisionPointRec(ui->ruler_start_p.rl, vr.rl)) {
		v2 end_p;
		if (ui->ruler_state == RS_START) end_p = mouse;
		else                             end_p = ui->ruler_stop_p;

		Color colour = colour_from_normalized(RULER_COLOUR);

		end_p          = clamp_v2_rect(end_p, vr);
		v2 pixel_delta = sub_v2(ui->ruler_start_p, end_p);
		v2 mm_delta    = mul_v2(pixels_to_mm, pixel_delta);

		DrawCircleV(ui->ruler_start_p.rl, 3, colour);
		DrawLineEx(end_p.rl, ui->ruler_start_p.rl, 2, colour);
		DrawCircleV(end_p.rl, 3, colour);

		buf.widx = 0;
		stream_append_f64(&buf, magnitude_v2(mm_delta), 100);
		stream_append_s8(&buf, s8(" mm"));

		v2 txt_p = ui->ruler_start_p;
		v2 txt_s = measure_text(ui->small_font, stream_to_s8(&buf));
		if (pixel_delta.y < 0) txt_p.y -= txt_s.y;
		if (pixel_delta.x < 0) txt_p.x -= txt_s.x;
		draw_outlined_text(ui->small_font, stream_to_s8(&buf), txt_p, 1, colour, BLACK);
	}
}

static v2
draw_beamformer_variable(BeamformerUI *ui, Arena arena, Variable *var, v2 at, v2 mouse,
                         f32 *hover_t, f32 space)
{
	Stream buf = arena_stream(&arena);
	stream_append_variable(&buf, var);

	Color colour = NORMALIZED_FG_COLOUR;
	v2 text_size = measure_text(ui->font, stream_to_s8(&buf));
	if (var->flags & V_INPUT) {
		Rect text_rect = {.pos = at, .size = text_size};
		text_rect = extend_rect_centered(text_rect, (v2){.x = 8});

		if (hover_rect(mouse, text_rect, hover_t)) {
			if (var->flags & V_TEXT) {
				InputState *is  = &ui->text_input_state;
				is->hot_rect    = text_rect;
				is->hot_font    = &ui->font;
			}
			ui->interaction.hot = var;
		}

		colour = colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, *hover_t));
	}
	return draw_text_limited(ui->font, stream_to_s8(&buf), at, colour, space, text_size.x);
}

#define LISTING_ITEM_PAD   12.0f
#define LISTING_INDENT     20.0f
#define LISTING_LINE_PAD    6.0f

static void
draw_variable_list(BeamformerUI *ui, Variable *group, Rect r, v2 mouse)
{
	/* TODO(rnp): limit the width of each element so that elements don't overlap */
	ASSERT(group->type == VT_GROUP);

	r = shrink_rect_centered(r, (v2){.x = 2 * UI_REGION_PAD, .y = UI_REGION_PAD});

	Variable *var = group->u.group.first;
	f32 x_off     = group->u.group.max_name_width;
	while (var) {
		s8 suffix   = {0};
		v2 at       = r.pos;
		f32 advance = 0;
		switch (var->type) {
		case VT_BEAMFORMER_VARIABLE: {
			BeamformerVariable *bv = (BeamformerVariable *)var;

			suffix   = bv->suffix;
			advance  = draw_text_limited(ui->font, var->name, at, NORMALIZED_FG_COLOUR,
			                             r.size.w + r.pos.x - at.x, var->name_width).y;
			at.x    += x_off + LISTING_ITEM_PAD;
			at.x    += draw_beamformer_variable(ui, ui->arena, var, at, mouse,
			                                    &var->hover_t, r.size.w + r.pos.x - at.x).x;

			while (var) {
				if (var->next) {
					var = var->next;
					break;
				}
				var       = var->parent;
				r.pos.x  -= LISTING_INDENT;
				r.size.x += LISTING_INDENT;
				x_off    += LISTING_INDENT;
			}
		} break;
		case VT_GROUP: {
			VariableGroup *g = &var->u.group;

			advance  = draw_beamformer_variable(ui, ui->arena, var, at, mouse, &var->hover_t,
			                                    r.size.w + r.pos.x - at.x).y;
			at.x    += x_off + LISTING_ITEM_PAD;
			if (g->expanded) {
				r.pos.x  += LISTING_INDENT;
				r.size.x -= LISTING_INDENT;
				x_off    -= LISTING_INDENT;
				var       = g->first;
			} else {
				BeamformerVariable *v = (BeamformerVariable *)g->first;

				ASSERT(!v || v->base.type == VT_BEAMFORMER_VARIABLE);
				/* NOTE(rnp): assume the suffix is the same for all elements */
				if (v) suffix = v->suffix;

				switch (g->type) {
				case VG_LIST: break;
				case VG_V2:
				case VG_V4: {
					at.x += draw_text_limited(ui->font, s8("{"), at, NORMALIZED_FG_COLOUR,
					                          r.size.w + r.pos.x - at.x,
					                          measure_text(ui->font, s8("{")).x).x;
					while (v) {
						at.x += draw_beamformer_variable(ui, ui->arena,
						             (Variable *)v, at, mouse, &v->base.hover_t,
						             r.size.w + r.pos.x - at.x).x;

						v = (BeamformerVariable *)v->base.next;
						if (v) at.x += draw_text_limited(ui->font, s8(", "),
						                   at, NORMALIZED_FG_COLOUR,
						                   r.size.w + r.pos.x - at.x,
						                   measure_text(ui->font, s8(", ")).x).x;
					}
					at.x += draw_text_limited(ui->font, s8("}"), at, NORMALIZED_FG_COLOUR,
					                          r.size.w + r.pos.x - at.x,
					                          measure_text(ui->font, s8("}")).x).x;
				} break;
				}

				var = var->next;
			}
		} break;
		default: INVALID_CODE_PATH;
		}

		if (suffix.len) {
			v2 suffix_s = measure_text(ui->font, suffix);
			if (r.size.w + r.pos.x - LISTING_ITEM_PAD - suffix_s.x > at.x) {
				v2 suffix_p = {.x = r.pos.x + r.size.w - suffix_s.w, .y = r.pos.y};
				draw_text(ui->font, suffix, suffix_p, NORMALIZED_FG_COLOUR);
			}
		}

		r.pos.y  += advance + LISTING_LINE_PAD;
		r.size.y -= advance + LISTING_LINE_PAD;
	}
}

static void
draw_compute_stats(BeamformerCtx *ctx, ComputeShaderStats *stats, Arena arena, Rect r)
{
	static s8 labels[CS_LAST] = {
		#define X(e, n, s, h, pn) [CS_##e] = s8(pn ":"),
		COMPUTE_SHADERS
		#undef X
	};

	BeamformerUI *ui = ctx->ui;

	Stream buf = stream_alloc(&arena, 64);

	r = shrink_rect_centered(r, (v2){.x = 2 * UI_REGION_PAD, .y = UI_REGION_PAD});
	v2 pos = r.pos;
	pos.y += r.size.h;

	s8   compute_total       = s8("Compute Total:");
	f32  compute_total_width = measure_text(ui->font, compute_total).x;
	f32  max_label_width     = compute_total_width;
	f32 *label_widths = alloc(&arena, f32, ARRAY_COUNT(labels));
	for (u32 i = 0; i < ARRAY_COUNT(labels); i++) {
		label_widths[i] = measure_text(ui->font, labels[i]).x;
		max_label_width = MAX(label_widths[i], max_label_width);
	}

	f32 compute_time_sum = 0;
	u32 stages = ctx->params->compute_stages_count;
	for (u32 i = 0; i < stages; i++) {
		u32 index  = ctx->params->compute_stages[i];
		pos.y     -= measure_text(ui->font, labels[index]).y;
		draw_text_limited(ui->font, labels[index], pos, colour_from_normalized(FG_COLOUR),
		                  r.size.w, label_widths[index]);

		stream_reset(&buf, 0);
		stream_append_f64_e(&buf, stats->times[index]);
		stream_append_s8(&buf, s8(" [s]"));
		v2 txt_fs = measure_text(ui->font, stream_to_s8(&buf));
		v2 rpos   = {.x = r.pos.x + max_label_width + LISTING_ITEM_PAD, .y = pos.y};
		draw_text_limited(ui->font, stream_to_s8(&buf), rpos, colour_from_normalized(FG_COLOUR),
		                  r.size.w - LISTING_ITEM_PAD - max_label_width, txt_fs.w);

		compute_time_sum += stats->times[index];
	}

	pos.y -= ui->font.baseSize;
	draw_text_limited(ui->font, compute_total, pos, colour_from_normalized(FG_COLOUR),
		          r.size.w, compute_total_width);

	stream_reset(&buf, 0);
	stream_append_f64_e(&buf, compute_time_sum);
	stream_append_s8(&buf, s8(" [s]"));
	v2 txt_fs = measure_text(ui->font, stream_to_s8(&buf));
	v2 rpos   = {.x = r.pos.x + max_label_width + LISTING_ITEM_PAD, .y = pos.y};
	draw_text_limited(ui->font, stream_to_s8(&buf), rpos, colour_from_normalized(FG_COLOUR),
	                  r.size.w - LISTING_ITEM_PAD - max_label_width, txt_fs.w);

	pos.y -= ui->font.baseSize;
	if (ctx->csctx.processing_compute) ui->progress_display_t_velocity += 65 * dt_for_frame;
	else                               ui->progress_display_t_velocity -= 45 * dt_for_frame;

	ui->progress_display_t_velocity = CLAMP(ui->progress_display_t_velocity, -10, 10);
	ui->progress_display_t += ui->progress_display_t_velocity * dt_for_frame;
	ui->progress_display_t  = CLAMP01(ui->progress_display_t);

	if (ui->progress_display_t > (1.0 / 255.0)) {
		s8 progress_text = s8("Compute Progress:");
		v2 txt_s         = measure_text(ui->font, progress_text);
		draw_text(ui->font, progress_text, pos,
		          fade(colour_from_normalized(FG_COLOUR), ui->progress_display_t));
		Rect prect = {
			.pos  = {.x = r.pos.x + txt_s.w + LISTING_ITEM_PAD, .y = pos.y},
			.size = {.w = r.size.w - txt_s.w - LISTING_ITEM_PAD, .h = txt_s.h}
		};
		prect = scale_rect_centered(prect, (v2){.x = 1, .y = 0.7});
		Rect fprect = prect;
		fprect.size.w *= ctx->csctx.processing_progress;
		DrawRectangleRounded(fprect.rl, 2, 0, fade(colour_from_normalized(HOVERED_COLOUR),
		                     ui->progress_display_t));
		DrawRectangleRoundedLinesEx(prect.rl, 2, 0, 4.0, colour_from_normalized(BG_COLOUR));
		prect = scale_rect_centered(prect, (v2){.x = 0.99, .y = 1});
		DrawRectangleRoundedLinesEx(prect.rl, 2, 0, 2.5, fade(BLACK, ui->progress_display_t));
	}
}

static void
draw_active_text_box(BeamformerUI *ui, Variable *var)
{
	InputState *is = &ui->text_input_state;
	Rect box       = is->rect;

	s8 text          = {.len = is->buf_len, .data = is->buf};
	v2 text_size     = measure_text(ui->font, text);
	v2 text_position = {.x = box.pos.x, .y = box.pos.y + (box.size.h - text_size.h) / 2};

	f32 cursor_width   = (is->cursor == is->buf_len) ? 16 : 4;
	f32 cursor_offset  = measure_text(ui->font, (s8){.data = text.data, .len = is->cursor}).w;
	cursor_offset     += text_position.x;

	box.size.w = MAX(box.size.w, text_size.w + cursor_width);
	Rect background = extend_rect_centered(box, (v2){.x = 12, .y = 8});
	box = extend_rect_centered(box, (v2){.x = 8, .y = 4});

	Rect cursor = {
		.pos  = {.x = cursor_offset, .y = text_position.y},
		.size = {.w = cursor_width,  .h = text_size.h},
	};

	v4 cursor_colour = FOCUSED_COLOUR;
	cursor_colour.a  = CLAMP01(is->cursor_blink_t);

	DrawRectangleRounded(background.rl, 0.2, 0, fade(BLACK, 0.8));
	DrawRectangleRounded(box.rl, 0.2, 0, colour_from_normalized(BG_COLOUR));
	draw_text(*is->font, text, text_position,
	          colour_from_normalized(lerp_v4(FG_COLOUR, HOVERED_COLOUR, var->hover_t)));
	DrawRectanglePro(cursor.rl, (Vector2){0}, 0, colour_from_normalized(cursor_colour));
}

static void
draw_variable(BeamformerUI *ui, Variable *var, Rect draw_rect, v2 mouse)
{
	if (var->type != VT_UI_REGION_SPLIT) {
		v2 shrink = {.x = UI_REGION_PAD, .y = UI_REGION_PAD};
		draw_rect = shrink_rect_centered(draw_rect, shrink);
	}

	BeginScissorMode(draw_rect.pos.x, draw_rect.pos.y, draw_rect.size.w, draw_rect.size.h);
	switch (var->type) {
	case VT_GROUP: {
		draw_variable_list(ui, var, draw_rect, mouse);
	} break;
	case VT_BEAMFORMER_FRAME_VIEW: {
		BeamformerFrameView *view = var->u.generic;
		if (beamformer_frame_view_frame(view)->ready_to_present)
			draw_beamformer_frame_view(ui, ui->arena, view, draw_rect, mouse);
	} break;
	case VT_COMPUTE_LATEST_STATS_VIEW: {
		ComputeStatsView *csv = &var->u.compute_stats_view;
		draw_compute_stats(csv->ctx, *(ComputeShaderStats **)csv->stats, ui->arena, draw_rect);
	} break;
	case VT_COMPUTE_STATS_VIEW: {
		ComputeStatsView *csv = &var->u.compute_stats_view;
		draw_compute_stats(csv->ctx, csv->stats, ui->arena, draw_rect);
	} break;
	case VT_UI_REGION_SPLIT: {
		RegionSplit *rs = &var->u.region_split;

		Rect split, hover;
		switch (rs->direction) {
		case RSD_VERTICAL: {
			split_rect_vertical(draw_rect, rs->fraction, 0, &split);
			split.pos.x  += UI_REGION_PAD;
			split.pos.y  -= UI_SPLIT_HANDLE_THICK / 2;
			split.size.h  = UI_SPLIT_HANDLE_THICK;
			split.size.w -= 2 * UI_REGION_PAD;
			hover = extend_rect_centered(split, (v2){.y = UI_REGION_PAD});
		} break;
		case RSD_HORIZONTAL: {
			split_rect_horizontal(draw_rect, rs->fraction, 0, &split);
			split.pos.x  -= UI_SPLIT_HANDLE_THICK / 2;
			split.pos.y  += UI_REGION_PAD;
			split.size.w  = UI_SPLIT_HANDLE_THICK;
			split.size.h -= 2 * UI_REGION_PAD;
			hover = extend_rect_centered(split, (v2){.x = UI_REGION_PAD});
		} break;
		}

		if (hover_rect(mouse, hover, &var->hover_t))
			ui->interaction.hot = var;

		v4 colour = HOVERED_COLOUR;
		colour.a  = var->hover_t;
		DrawRectangleRounded(split.rl, 0.6, 0, colour_from_normalized(colour));
	} break;
	default: break;
	}
	EndScissorMode();
}

static void
draw_ui_regions(BeamformerUI *ui, Rect window, v2 mouse)
{
	struct region_stack_item {
		Variable *var;
		Rect      rect;
	} *region_stack;

	TempArena arena_savepoint = begin_temp_arena(&ui->arena);
	i32 stack_index = 0;

	region_stack = alloc(&ui->arena, typeof(*region_stack), 256);
	region_stack[0].var  = ui->regions;
	region_stack[0].rect = window;

	while (stack_index != -1) {
		struct region_stack_item *rsi = region_stack + stack_index--;
		Rect rect = rsi->rect;
		draw_variable(ui, rsi->var, rect, mouse);

		if (rsi->var->type == VT_UI_REGION_SPLIT) {
			Rect first, second;
			RegionSplit *rs = &rsi->var->u.region_split;
			switch (rs->direction) {
			case RSD_VERTICAL: {
				split_rect_vertical(rect, rs->fraction, &first, &second);
			} break;
			case RSD_HORIZONTAL: {
				split_rect_horizontal(rect, rs->fraction, &first, &second);
			} break;
			}

			stack_index++;
			region_stack[stack_index].var  = rs->right;
			region_stack[stack_index].rect = second;
			stack_index++;
			region_stack[stack_index].var  = rs->left;
			region_stack[stack_index].rect = first;
		}

		ASSERT(stack_index < 256);
	}

	end_temp_arena(arena_savepoint);
}

static void
ui_store_variable_base(VariableType type, void *store, void *new_value, void *limits)
{
	switch (type) {
	case VT_B32: {
		*(b32 *)store = *(b32 *)new_value;
	} break;
	case VT_F32: {
		v2 *lim = limits;
		f32 val = *(f32 *)new_value;
		*(f32 *)store = CLAMP(val, lim->x, lim->y);
	} break;
	default: INVALID_CODE_PATH;
	}
}

static void
ui_store_variable(Variable *var, void *new_value)
{
	switch (var->type) {
	case VT_F32: {
		v2 limits = {.x = -F32_INFINITY, .y = F32_INFINITY};
		ui_store_variable_base(VT_F32, &var->u.f32, new_value, &limits);
	} break;
	case VT_BEAMFORMER_VARIABLE: {
		BeamformerVariable *bv = (BeamformerVariable *)var;
		ui_store_variable_base(bv->subtype, var->u.generic, new_value, &bv->params.limits);
	} break;
	default: INVALID_CODE_PATH;
	}
}

static void
scroll_interaction_base(VariableType type, void *store, f32 delta)
{
	switch (type) {
	case VT_B32: { *(b32 *)store  = !*(b32 *)store; } break;
	case VT_F32: { *(f32 *)store += delta;          } break;
	case VT_I32: { *(i32 *)store += delta;          } break;
	default: INVALID_CODE_PATH;
	}
}

static void
scroll_interaction(Variable *var, f32 delta)
{
	switch (var->type) {
	case VT_BEAMFORMER_VARIABLE: {
		BeamformerVariable *bv = (BeamformerVariable *)var;
		scroll_interaction_base(bv->subtype, var->u.generic, delta * bv->params.scroll_scale);
		ui_store_variable(var, var->u.generic);
	} break;
	default: scroll_interaction_base(var->type, &var->u, delta); break;
	}
}

static void
begin_text_input(InputState *is, Variable *var, v2 mouse)
{
	Stream s = {.cap = ARRAY_COUNT(is->buf), .data = is->buf};
	stream_append_variable(&s, var);
	is->buf_len = s.widx;
	is->rect    = is->hot_rect;
	is->font    = is->hot_font;

	/* NOTE: extra offset to help with putting a cursor at idx 0 */
	#define TEXT_HALF_CHAR_WIDTH 10
	f32 hover_p = CLAMP01((mouse.x - is->rect.pos.x) / is->rect.size.w);
	f32 x_off = TEXT_HALF_CHAR_WIDTH, x_bounds = is->rect.size.w * hover_p;
	i32 i;
	for (i = 0; i < is->buf_len && x_off < x_bounds; i++) {
		/* NOTE: assumes font glyphs are ordered ASCII */
		i32 idx  = is->buf[i] - 0x20;
		x_off   += is->font->glyphs[idx].advanceX;
		if (is->font->glyphs[idx].advanceX == 0)
			x_off += is->font->recs[idx].width;
	}
	is->cursor = i;
}

static void
end_text_input(InputState *is, Variable *var)
{
	f32 scale = 1;
	if (var->type == VT_BEAMFORMER_VARIABLE) {
		BeamformerVariable *bv = (BeamformerVariable *)var;
		ASSERT(bv->subtype == VT_F32);
		scale = bv->params.display_scale;
		var->hover_t = 0;
	}
	f32 value = parse_f64((s8){.len = is->buf_len, .data = is->buf}) / scale;
	ui_store_variable(var, &value);
}

static void
update_text_input(InputState *is, Variable *var)
{
	ASSERT(is->cursor != -1);

	is->cursor_blink_t += is->cursor_blink_scale * dt_for_frame;
	if (is->cursor_blink_t >= 1) is->cursor_blink_scale = -1.5f;
	if (is->cursor_blink_t <= 0) is->cursor_blink_scale =  1.5f;

	var->hover_t -= 2 * HOVER_SPEED * dt_for_frame;
	var->hover_t  = CLAMP01(var->hover_t);

	/* NOTE: handle multiple input keys on a single frame */
	i32 key = GetCharPressed();
	while (key > 0) {
		if ((is->buf_len == ARRAY_COUNT(is->buf)) || (is->cursor == ARRAY_COUNT(is->buf) - 1))
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
		if (is->cursor < ARRAY_COUNT(is->buf) - 1) {
			mem_move(is->buf + is->cursor + 1,
			         is->buf + is->cursor,
			         is->buf_len - is->cursor);
		}
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
display_interaction_end(BeamformerUI *ui)
{
	b32 is_hot    = ui->interaction.hot_type == IT_DISPLAY;
	b32 is_active = ui->interaction.type     == IT_DISPLAY;
	if ((is_active && is_hot) || ui->ruler_state == RS_HOLD)
		return;
	ui->ruler_state = RS_NONE;
}

static void
display_interaction(BeamformerUI *ui, v2 mouse)
{
	b32 mouse_left_pressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
	b32 mouse_right_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
	b32 is_hot              = ui->interaction.hot_type == IT_DISPLAY;
	b32 is_active           = ui->interaction.type     == IT_DISPLAY;

	if (mouse_left_pressed && is_active) {
		ui->ruler_state++;
		switch (ui->ruler_state) {
		case RS_START: ui->ruler_start_p = mouse; break;
		case RS_HOLD:  ui->ruler_stop_p  = mouse; break;
		default:
			ui->ruler_state = RS_NONE;
			break;
		}
	} else if (mouse_right_pressed && is_hot) {
		ui->ruler_state = RS_NONE;
	}
}

static void
scale_bar_interaction(BeamformerCtx *ctx, v2 mouse)
{
	BeamformerUI *ui        = ctx->ui;
	InteractionState *is    = &ui->interaction;
	ScaleBar *sb            = is->active->u.generic;
	b32 mouse_left_pressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
	b32 mouse_right_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
	f32 mouse_wheel         = GetMouseWheelMoveV().y;

	if (mouse_left_pressed) {
		if (sb->zoom_starting_point.x == F32_INFINITY) {
			sb->zoom_starting_point = sub_v2(mouse, sb->screen_offset);
		} else {
			v2 relative_mouse = sub_v2(mouse, sb->screen_offset);
			f32 min = magnitude_v2(mul_v2(sb->zoom_starting_point, sb->screen_space_to_value));
			f32 max = magnitude_v2(mul_v2(relative_mouse,          sb->screen_space_to_value));
			if (min > max) { f32 tmp = min; min = max; max = tmp; }

			min += *sb->min_value;
			max += *sb->min_value;

			/* TODO(rnp): SLL_* macros */
			v2_sll *savepoint = ui->scale_bar_savepoint_freelist;
			if (!savepoint) savepoint = push_struct(&ui->arena, v2_sll);
			ui->scale_bar_savepoint_freelist = savepoint->next;

			savepoint->v.x      = *sb->min_value;
			savepoint->v.y      = *sb->max_value;
			savepoint->next     = sb->savepoint_stack;
			sb->savepoint_stack = savepoint;

			*sb->min_value = MAX(min, sb->limits.x);
			*sb->max_value = MIN(max, sb->limits.y);

			sb->zoom_starting_point = (v2){.x = F32_INFINITY, .y = F32_INFINITY};
			ui->flush_params = 1;
		}
	}

	if (mouse_right_pressed) {
		v2_sll *savepoint = sb->savepoint_stack;
		if (savepoint) {
			*sb->min_value = savepoint->v.x;
			*sb->max_value = savepoint->v.y;

			sb->savepoint_stack = savepoint->next;
			savepoint->next     = ui->scale_bar_savepoint_freelist;
			ui->scale_bar_savepoint_freelist = savepoint;

			ui->flush_params = 1;
		}
		sb->zoom_starting_point = (v2){.x = F32_INFINITY, .y = F32_INFINITY};
	}

	if (mouse_wheel) {
		*sb->min_value += mouse_wheel * sb->scroll_scale.x;
		*sb->max_value += mouse_wheel * sb->scroll_scale.y;
		*sb->min_value  = MAX(sb->limits.x, *sb->min_value);
		*sb->max_value  = MIN(sb->limits.y, *sb->max_value);
		ui->flush_params = 1;
	}
}

static void
ui_begin_interact(BeamformerUI *ui, BeamformerInput *input, b32 scroll, b32 mouse_left_pressed)
{
	InteractionState *is = &ui->interaction;
	if (is->hot_type != IT_NONE) {
		is->type = is->hot_type;
	} else if (is->hot) {
		switch (is->hot->type) {
		case VT_NULL:  is->type = IT_NOP; break;
		case VT_B32:   is->type = IT_SET; break;
		case VT_GROUP: is->type = IT_SET; break;
		case VT_UI_REGION_SPLIT: is->type = IT_DRAG; break;
		case VT_BEAMFORMER_VARIABLE: {
			BeamformerVariable *bv = (BeamformerVariable *)is->hot;
			if (bv->subtype == VT_B32) {
				is->type = IT_SET;
				break;
			}
		} /* FALLTHROUGH */
		case VT_F32: {
			if (scroll) {
				is->type = IT_SCROLL;
			} else if (mouse_left_pressed && is->hot->flags & V_TEXT) {
				is->type = IT_TEXT;
				begin_text_input(&ui->text_input_state, is->hot, input->mouse);
			}
		} break;
		default: INVALID_CODE_PATH;
		}
	}
	if (is->type != IT_NONE) {
		is->active = is->hot;
		if ((iptr)is->hot == (iptr)ui->scratch_variables)
			ui->scratch_variable = ui->scratch_variables + 1;
		else
			ui->scratch_variable = ui->scratch_variables + 0;
	}
}

static void
ui_end_interact(BeamformerCtx *ctx, v2 mouse)
{
	BeamformerUI *ui = ctx->ui;
	InteractionState *is = &ui->interaction;
	switch (is->type) {
	case IT_NOP:  break;
	case IT_SET: {
		switch (is->active->type) {
		case VT_GROUP: {
			is->active->u.group.expanded = !is->active->u.group.expanded;
		} break;
		case VT_B32: {
			is->active->u.b32 = !is->active->u.b32;
		} break;
		case VT_BEAMFORMER_VARIABLE: {
			ASSERT(((BeamformerVariable *)is->active)->subtype == VT_B32);
			b32 *val = is->active->u.generic;
			*val     = !(*val);
		} break;
		default: INVALID_CODE_PATH;
		}
	} break;
	case IT_DISPLAY: display_interaction_end(ui); /* FALLTHROUGH */
	case IT_SCROLL:  scroll_interaction(is->active, GetMouseWheelMoveV().y); break;
	case IT_TEXT:    end_text_input(&ui->text_input_state, is->active);      break;
	case IT_SCALE_BAR: break;
	case IT_DRAG:      break;
	default: INVALID_CODE_PATH;
	}

	if (is->active->flags & V_CAUSES_COMPUTE)
		ui->flush_params = 1;
	if (is->active->flags & V_GEN_MIPMAPS && ctx->fsctx.output.texture.id)
		ctx->fsctx.gen_mipmaps = 1;

	is->type   = IT_NONE;
	is->active = 0;
}

static void
ui_interact(BeamformerCtx *ctx, BeamformerInput *input)
{
	BeamformerUI *ui        = ctx->ui;
	InteractionState *is    = &ui->interaction;
	b32 mouse_left_pressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
	b32 mouse_right_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
	b32 wheel_moved         = GetMouseWheelMoveV().y != 0;
	if (mouse_right_pressed || mouse_left_pressed || wheel_moved) {
		if (is->type != IT_NONE)
			ui_end_interact(ctx, input->mouse);
		ui_begin_interact(ui, input, wheel_moved, mouse_left_pressed);
	}

	if (IsKeyPressed(KEY_ENTER) && is->type == IT_TEXT)
		ui_end_interact(ctx, input->mouse);

	switch (is->type) {
	case IT_NONE: break;
	case IT_NOP:  break;
	case IT_DISPLAY: display_interaction(ui, input->mouse); break;
	case IT_SCROLL:  ui_end_interact(ctx, input->mouse);    break;
	case IT_SET:     ui_end_interact(ctx, input->mouse);    break;
	case IT_TEXT:    update_text_input(&ui->text_input_state, is->active); break;
	case IT_DRAG: {
		if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
			ui_end_interact(ctx, input->mouse);
		} else {
			v2 ws     = (v2){.w = ctx->window_size.w, .h = ctx->window_size.h};
			v2 dMouse = sub_v2(input->mouse, input->last_mouse);
			dMouse    = mul_v2(dMouse, (v2){.x = 1.0f / ws.w, .y = 1.0f / ws.h});

			switch (is->active->type) {
			case VT_UI_REGION_SPLIT: {
				f32 min_fraction;
				RegionSplit *rs = &is->active->u.region_split;
				switch (rs->direction) {
				case RSD_VERTICAL: {
					min_fraction  = (UI_SPLIT_HANDLE_THICK + 0.5 * UI_REGION_PAD) / ws.h;
					rs->fraction += dMouse.y;
				} break;
				case RSD_HORIZONTAL: {
					min_fraction  = (UI_SPLIT_HANDLE_THICK + 0.5 * UI_REGION_PAD) / ws.w;
					rs->fraction += dMouse.x;
				} break;
				}
				rs->fraction = CLAMP(rs->fraction, min_fraction, 1 - min_fraction);
			} break;
			default: break;
			}

			if (is->active != is->hot) {
				is->active->hover_t += HOVER_SPEED * dt_for_frame;
				is->active->hover_t  = CLAMP01(is->active->hover_t);
			}
		}
	} break;
	case IT_SCALE_BAR: scale_bar_interaction(ctx, input->mouse); break;
	}

	is->hot_type = IT_NONE;
	is->hot      = 0;
}

static void
ui_init(BeamformerCtx *ctx, Arena store)
{
	/* NOTE(rnp): store the ui at the base of the passed in arena and use the rest for
	 * temporary allocations within the ui. If needed we can recall this function to
	 * completely clear the ui state. The is that if we store pointers to static data
	 * such as embedded font data we will need to reset them when the executable reloads.
	 * We could also build some sort of ui structure here and store it then iterate over
	 * it to actually draw the ui. If we reload we may have changed it so we should
	 * rebuild it */

	BeamformerUI *ui = ctx->ui;

	/* NOTE: unload old fonts from the GPU */
	if (ui) {
		UnloadFont(ui->font);
		UnloadFont(ui->small_font);
	}

	ui = ctx->ui = push_struct(&store, typeof(*ui));
	ui->arena = store;

	/* TODO: build these into the binary */
	ui->font       = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 28, 0, 0);
	ui->small_font = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 22, 0, 0);


	ui->scratch_variable = ui->scratch_variables + 0;
	Variable *split = ui->regions   = add_ui_split(0, &ui->arena, s8("UI Root"), 0.35,
	                                               RSD_HORIZONTAL, ui->font);
	split->u.region_split.left      = add_ui_split(split, &ui->arena, s8(""), 0.8,
	                                               RSD_VERTICAL, ui->font);
	split->u.region_split.right     = add_beamformer_frame_view(split, &ui->arena,
	                                                            FVT_LATEST, ui->font);

	BeamformerFrameView *bv         = split->u.region_split.right->u.generic;
	bv->store                       = &ui->latest_frame;
	bv->lateral_scale_bar.min_value = &ui->params.output_min_coordinate.x;
	bv->lateral_scale_bar.max_value = &ui->params.output_max_coordinate.x;
	bv->axial_scale_bar.min_value   = &ui->params.output_min_coordinate.z;
	bv->axial_scale_bar.max_value   = &ui->params.output_max_coordinate.z;

	bv->lateral_scale_bar.limits = (v2){.x = -1, .y = 1};
	bv->axial_scale_bar.limits   = (v2){.x =  0, .y = 1};

	bv->lateral_scale_bar.scroll_scale = (v2){.x = -0.5e-3, .y = 0.5e-3};
	bv->axial_scale_bar.scroll_scale   = (v2){.x =  0,      .y = 1e-3};

	bv->lateral_scale_bar.zoom_starting_point = (v2){.x = F32_INFINITY, .y = F32_INFINITY};
	bv->axial_scale_bar.zoom_starting_point   = (v2){.x = F32_INFINITY, .y = F32_INFINITY};

	/* TODO(rnp): hack: each view needs their own cut down fragment shader context */
	bv->ctx = &ctx->fsctx;

	split = split->u.region_split.left;
	split->u.region_split.left  = add_beamformer_parameters_view(split, ctx);
	split->u.region_split.right = add_variable(split, &ui->arena, s8(""), 0,
	                                           VT_COMPUTE_LATEST_STATS_VIEW, ui->font);
	ComputeStatsView *compute_stats = &split->u.region_split.right->u.compute_stats_view;
	compute_stats->ctx   = ctx;
	compute_stats->stats = &ui->latest_compute_stats;

	ctx->ui_read_params = 1;
}

static void
validate_ui_parameters(BeamformerUIParameters *p)
{
	if (p->output_min_coordinate.x > p->output_max_coordinate.x)
		SWAP(p->output_min_coordinate.x, p->output_max_coordinate.x)
	if (p->output_min_coordinate.z > p->output_max_coordinate.z)
		SWAP(p->output_min_coordinate.z, p->output_max_coordinate.z)
}

static void
draw_ui(BeamformerCtx *ctx, BeamformerInput *input, BeamformFrame *frame_to_draw,
        ComputeShaderStats *latest_compute_stats)
{
	BeamformerUI *ui = ctx->ui;
	ui->latest_frame = frame_to_draw;
	ui->latest_compute_stats = latest_compute_stats;

	/* TODO(rnp): there should be a better way of detecting this */
	if (ctx->ui_read_params) {
		mem_copy(&ctx->params->raw.output_min_coordinate, &ui->params, sizeof(ui->params));
		ui->flush_params    = 0;
		ctx->ui_read_params = 0;
	}

	/* NOTE: process interactions first because the user interacted with
	 * the ui that was presented last frame */
	ui_interact(ctx, input);

	if (ui->flush_params) {
		validate_ui_parameters(&ui->params);
		if (!ctx->csctx.processing_compute) {
			mem_copy(&ui->params, &ctx->params->raw.output_min_coordinate, sizeof(ui->params));
			ui->flush_params    = 0;
			ctx->params->upload = 1;
			ctx->start_compute  = 1;
		}
	}

	BeginDrawing();
		ClearBackground(colour_from_normalized(BG_COLOUR));

		v2 mouse         = input->mouse;
		Rect window_rect = {.size = {.w = ctx->window_size.w, .h = ctx->window_size.h}};

		draw_ui_regions(ui, window_rect, mouse);
		if (ui->interaction.type == IT_TEXT)
			draw_active_text_box(ui, ui->interaction.active);
	EndDrawing();
}
