function void
fill_kronecker_sub_matrix(i32 *out, i32 out_stride, i32 scale, i32 *b, uv2 b_dim)
{
	f32x4 vscale = dup_f32x4(scale);
	for (u32 i = 0; i < b_dim.y; i++) {
		for (u32 j = 0; j < b_dim.x; j += 4, b += 4) {
			f32x4 vb = cvt_i32x4_f32x4(load_i32x4(b));
			store_i32x4(cvt_f32x4_i32x4(mul_f32x4(vscale, vb)), out + j);
		}
		out += out_stride;
	}
}

/* NOTE: this won't check for valid space/etc and assumes row major order */
function void
kronecker_product(i32 *out, i32 *a, uv2 a_dim, i32 *b, uv2 b_dim)
{
	uv2 out_dim = {.x = a_dim.x * b_dim.x, .y = a_dim.y * b_dim.y};
	ASSERT(out_dim.y % 4 == 0);
	for (u32 i = 0; i < a_dim.y; i++) {
		i32 *vout = out;
		for (u32 j = 0; j < a_dim.x; j++, a++) {
			fill_kronecker_sub_matrix(vout, out_dim.y, *a, b, b_dim);
			vout += b_dim.y;
		}
		out += out_dim.y * b_dim.x;
	}
}

/* NOTE/TODO: to support even more hadamard sizes use the Paley construction */
function i32 *
make_hadamard_transpose(Arena *a, u32 dim)
{
	read_only local_persist	i32 hadamard_12_12_transpose[] = {
		1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
		1, -1, -1,  1, -1, -1, -1,  1,  1,  1, -1,  1,
		1,  1, -1, -1,  1, -1, -1, -1,  1,  1,  1, -1,
		1, -1,  1, -1, -1,  1, -1, -1, -1,  1,  1,  1,
		1,  1, -1,  1, -1, -1,  1, -1, -1, -1,  1,  1,
		1,  1,  1, -1,  1, -1, -1,  1, -1, -1, -1,  1,
		1,  1,  1,  1, -1,  1, -1, -1,  1, -1, -1, -1,
		1, -1,  1,  1,  1, -1,  1, -1, -1,  1, -1, -1,
		1, -1, -1,  1,  1,  1, -1,  1, -1, -1,  1, -1,
		1, -1, -1, -1,  1,  1,  1, -1,  1, -1, -1,  1,
		1,  1, -1, -1, -1,  1,  1,  1, -1,  1, -1, -1,
		1, -1,  1, -1, -1, -1,  1,  1,  1, -1,  1, -1,
	};

	read_only local_persist i32 hadamard_20_20_transpose[] = {
		1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
		1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1,
		1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1,
		1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,
		1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,
		1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1,
		1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1,
		1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1,
		1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1,
		1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,
		1, -1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1,
		1,  1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,
		1, -1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1,
		1,  1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,
		1,  1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,
		1,  1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,
		1,  1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,
		1, -1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1,
		1, -1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1,
		1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1, -1,  1, -1,  1,  1,  1,  1, -1, -1,
	};

	i32 *result = 0;

	b32 power_of_2     = ISPOWEROF2(dim);
	b32 multiple_of_12 = dim % 12 == 0;
	b32 multiple_of_20 = dim % 20 == 0;
	iz elements        = dim * dim;

	u32 base_dim = 0;
	if (power_of_2) {
		base_dim  = dim;
	} else if (multiple_of_20 && ISPOWEROF2(dim / 20)) {
		base_dim  = 20;
		dim      /= 20;
	} else if (multiple_of_12 && ISPOWEROF2(dim / 12)) {
		base_dim  = 12;
		dim      /= 12;
	}

	if (ISPOWEROF2(dim) && base_dim && arena_capacity(a, i32) >= elements * (1 + (dim != base_dim))) {
		result = push_array(a, i32, elements);

		Arena tmp = *a;
		i32 *m = dim == base_dim ? result : push_array(&tmp, i32, elements);

		#define IND(i, j) ((i) * dim + (j))
		m[0] = 1;
		for (u32 k = 1; k < dim; k *= 2) {
			for (u32 i = 0; i < k; i++) {
				for (u32 j = 0; j < k; j++) {
					i32 val = m[IND(i, j)];
					m[IND(i + k, j)]     =  val;
					m[IND(i, j + k)]     =  val;
					m[IND(i + k, j + k)] = -val;
				}
			}
		}
		#undef IND

		i32 *m2 = 0;
		uv2 m2_dim;
		switch (base_dim) {
		case 12:{ m2 = hadamard_12_12_transpose; m2_dim = (uv2){{12, 12}}; }break;
		case 20:{ m2 = hadamard_20_20_transpose; m2_dim = (uv2){{20, 20}}; }break;
		}
		if (m2) kronecker_product(result, m, (uv2){{dim, dim}}, m2, m2_dim);
	}

	return result;
}

function b32
uv2_equal(uv2 a, uv2 b)
{
	return a.x == b.x && a.y == b.y;
}

function b32
uv3_equal(uv3 a, uv3 b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

function v2
clamp_v2_rect(v2 v, Rect r)
{
	v2 result = v;
	result.x = CLAMP(v.x, r.pos.x, r.pos.x + r.size.x);
	result.y = CLAMP(v.y, r.pos.y, r.pos.y + r.size.y);
	return result;
}

function v2
v2_scale(v2 a, f32 scale)
{
	v2 result;
	result.x = a.x * scale;
	result.y = a.y * scale;
	return result;
}

function v2
v2_add(v2 a, v2 b)
{
	v2 result;
	result.x = a.x + b.x;
	result.y = a.y + b.y;
	return result;
}

function v2
v2_sub(v2 a, v2 b)
{
	v2 result = v2_add(a, v2_scale(b, -1.0f));
	return result;
}

function v2
v2_mul(v2 a, v2 b)
{
	v2 result;
	result.x = a.x * b.x;
	result.y = a.y * b.y;
	return result;
}

function v2
v2_div(v2 a, v2 b)
{
	v2 result;
	result.x = a.x / b.x;
	result.y = a.y / b.y;
	return result;
}

function v2
v2_floor(v2 a)
{
	v2 result;
	result.x = (i32)a.x;
	result.y = (i32)a.y;
	return result;
}

function f32
v2_magnitude(v2 a)
{
	f32 result = sqrt_f32(a.x * a.x + a.y * a.y);
	return result;
}


function v3
cross(v3 a, v3 b)
{
	v3 result;
	result.x = a.y * b.z - a.z * b.y;
	result.y = a.z * b.x - a.x * b.z;
	result.z = a.x * b.y - a.y * b.x;
	return result;
}

function v3
v3_scale(v3 a, f32 scale)
{
	v3 result;
	result.x = scale * a.x;
	result.y = scale * a.y;
	result.z = scale * a.z;
	return result;
}

function v3
v3_add(v3 a, v3 b)
{
	v3 result;
	result.x = a.x + b.x;
	result.y = a.y + b.y;
	result.z = a.z + b.z;
	return result;
}

function v3
v3_sub(v3 a, v3 b)
{
	v3 result = v3_add(a, v3_scale(b, -1.0f));
	return result;
}

function f32
v3_dot(v3 a, v3 b)
{
	f32 result = a.x * b.x + a.y * b.y + a.z * b.z;
	return result;
}

function f32
v3_length_squared(v3 a)
{
	f32 result = v3_dot(a, a);
	return result;
}

function v3
v3_normalize(v3 a)
{
	v3 result = v3_scale(a, 1.0f / sqrt_f32(v3_length_squared(a)));
	return result;
}

function uv4
uv4_from_u32_array(u32 v[4])
{
	uv4 result;
	result.E[0] = v[0];
	result.E[1] = v[1];
	result.E[2] = v[2];
	result.E[3] = v[3];
	return result;
}

function b32
uv4_equal(uv4 a, uv4 b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

function v4
v4_from_f32_array(f32 v[4])
{
	v4 result;
	result.E[0] = v[0];
	result.E[1] = v[1];
	result.E[2] = v[2];
	result.E[3] = v[3];
	return result;
}

function v4
v4_scale(v4 a, f32 scale)
{
	v4 result;
	result.x = scale * a.x;
	result.y = scale * a.y;
	result.z = scale * a.z;
	result.w = scale * a.w;
	return result;
}

function v4
v4_add(v4 a, v4 b)
{
	v4 result;
	result.x = a.x + b.x;
	result.y = a.y + b.y;
	result.z = a.z + b.z;
	result.w = a.w + b.w;
	return result;
}

function v4
v4_sub(v4 a, v4 b)
{
	v4 result = v4_add(a, v4_scale(b, -1));
	return result;
}

function f32
v4_dot(v4 a, v4 b)
{
	f32 result = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	return result;
}

function v4
v4_lerp(v4 a, v4 b, f32 t)
{
	v4 result = v4_add(a, v4_scale(v4_sub(b, a), t));
	return result;
}

function v4
m4_row(m4 a, u32 row)
{
	v4 result;
	result.E[0] = a.c[0].E[row];
	result.E[1] = a.c[1].E[row];
	result.E[2] = a.c[2].E[row];
	result.E[3] = a.c[3].E[row];
	return result;
}

function v4
m4_column(m4 a, u32 column)
{
	v4 result = a.c[column];
	return result;
}

function m4
m4_mul(m4 a, m4 b)
{
	m4 result;
	for (u32 i = 0; i < countof(result.E); i++) {
		u32 base = i / 4;
		u32 sub  = i % 4;
		v4 v1 = m4_row(a, base);
		v4 v2 = m4_column(b, sub);
		result.E[i] = v4_dot(v1, v2);
	}
	return result;
}

function m4
m4_rotation_about_y(f32 turns)
{
	f32 sa = sin_f32(turns * 2 * PI);
	f32 ca = cos_f32(turns * 2 * PI);
	m4 result;
	result.c[0] = (v4){{ca, 0, -sa, 0}};
	result.c[1] = (v4){{0,  1,  0,  0}};
	result.c[2] = (v4){{sa, 0,  ca, 0}};
	result.c[3] = (v4){{0,  0,  0,  1}};
	return result;
}

function m4
y_aligned_volume_transform(v3 extent, v3 translation, f32 rotation_turns)
{
	m4 S;
	S.c[0] = (v4){{extent.x, 0,        0,        0}};
	S.c[1] = (v4){{0,        extent.y, 0,        0}};
	S.c[2] = (v4){{0,        0,        extent.z, 0}};
	S.c[3] = (v4){{0,        0,        0,        1}};

	m4 R = m4_rotation_about_y(rotation_turns);

	m4 T;
	T.c[0] = (v4){{1, 0, 0, translation.x}};
	T.c[1] = (v4){{0, 1, 0, translation.y}};
	T.c[2] = (v4){{0, 0, 1, translation.z}};
	T.c[3] = (v4){{0, 0, 0, 1}};

	m4 result = m4_mul(m4_mul(R, S), T);
	return result;
}

function v4
m4_mul_v4(m4 a, v4 v)
{
	v4 result;
	result.x = v4_dot(m4_row(a, 0), v);
	result.y = v4_dot(m4_row(a, 1), v);
	result.z = v4_dot(m4_row(a, 2), v);
	result.w = v4_dot(m4_row(a, 3), v);
	return result;
}

function m4
perspective_projection(f32 n, f32 f, f32 fov, f32 aspect)
{
	m4 result;
	f32 t = tan_f32(fov / 2.0f);
	f32 r = t * aspect;
	f32 a = -(f + n) / (f - n);
	f32 b = -2 * f * n / (f - n);
	result.c[0] = (v4){{1 / r, 0,     0,  0}};
	result.c[1] = (v4){{0,     1 / t, 0,  0}};
	result.c[2] = (v4){{0,     0,     a, -1}};
	result.c[3] = (v4){{0,     0,     b,  0}};
	return result;
}

function m4
camera_look_at(v3 camera, v3 point)
{
	v3 orthogonal = {{0, 1.0f, 0}};
	v3 normal     = v3_normalize(v3_sub(camera, point));
	v3 right      = cross(orthogonal, normal);
	v3 up         = cross(normal,     right);

	v3 translate;
	camera      = v3_sub((v3){0}, camera);
	translate.x = v3_dot(camera, right);
	translate.y = v3_dot(camera, up);
	translate.z = v3_dot(camera, normal);

	m4 result;
	result.c[0] = (v4){{right.x,     up.x,        normal.x,    0}};
	result.c[1] = (v4){{right.y,     up.y,        normal.y,    0}};
	result.c[2] = (v4){{right.z,     up.z,        normal.z,    0}};
	result.c[3] = (v4){{translate.x, translate.y, translate.z, 1}};
	return result;
}
