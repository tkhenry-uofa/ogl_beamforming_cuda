/* See LICENSE for license details. */
#include <stdio.h>

static void *
mem_clear(u8 *p, u8 c, size len)
{
	while (len) p[--len] = c;
	return p;
}

static void
mem_move(u8 *src, u8 *dest, size n)
{
	if (dest < src) while (n) { *dest++ = *src++; n--; }
	else            while (n) { n--; dest[n] = src[n]; }
}

#define alloc(a, t, n)  (t *)alloc_(a, sizeof(t), _Alignof(t), n)
static void *
alloc_(Arena *a, size len, size align, size count)
{
	size padding   = -(uintptr_t)a->beg & (align - 1);
	size available = a->end - a->beg - padding;
	if (available < 0 || count > available / len)
		ASSERT(0 && "arena OOM\n");
	void *p = a->beg + padding;
	a->beg += padding + count * len;
	/* TODO: Performance? */
	return mem_clear(p, 0, count * len);
}

static Stream
stream_alloc(Arena *a, size cap)
{
	Stream result = {.cap = cap};
	result.data = alloc(a, u8, cap);
	return result;
}

static s8
stream_to_s8(Stream s)
{
	ASSERT(!s.errors);
	s8 result = {.len = s.widx, .data = s.data};
	return result;
}

static void
stream_append_byte(Stream *s, u8 b)
{
	s->errors |= s->widx + 1 > s->cap;
	if (!s->errors)
		s->data[s->widx++] = b;
}

static void
stream_append_s8(Stream *s, s8 str)
{
	s->errors |= (s->cap - s->widx) <= str.len;
	if (!s->errors) {
		for (size i = 0; i < str.len; i++)
			s->data[s->widx++] = str.data[i];
	}
}

static void
stream_append_s8_array(Stream *s, s8 *strs, size count)
{
	while (count > 0) {
		stream_append_s8(s, *strs);
		strs++;
		count--;
	}
}

static void
stream_append_u64(Stream *s, u64 n)
{
	u8 tmp[64];
	u8 *end = tmp + sizeof(tmp);
	u8 *beg = end;
	do { *--beg = '0' + (n % 10); } while (n /= 10);
	stream_append_s8(s, (s8){.len = end - beg, .data = beg});
}

static void
stream_append_i64(Stream *s, i64 n)
{
	if (n < 0) {
		stream_append_s8(s, s8("-"));
		n *= -1;
	}
	stream_append_u64(s, n);
}

static void
stream_append_f64(Stream *s, f64 f, i64 prec)
{
	if (f < 0) {
		stream_append_byte(s, '-');
		f *= -1;
	}

	/* NOTE: round last digit */
	f += 0.5f / prec;

	if (f >= (f64)(-1UL >> 1)) {
		stream_append_s8(s, s8("inf"));
	} else {
		u64 integral = f;
		u64 fraction = (f - integral) * prec;
		stream_append_u64(s, integral);
		stream_append_byte(s, '.');
		for (i64 i = prec / 10; i > 1; i /= 10) {
			if (i > fraction)
				stream_append_byte(s, '0');
		}
		stream_append_u64(s, fraction);
	}
}

static void
stream_append_f32_e(Stream *s, f32 f)
{
	if (f < 0) {
		stream_append_s8(s, s8("-"));
		f *= -1;
	}
	/* TODO */
	size remaining = s->cap - s->widx;
	s->errors |= remaining <= snprintf(0, 0, "%0.02e", f);
	if (!s->errors)
		s->widx += snprintf((char *)(s->data + s->widx), remaining, "%0.02e", f);
}

static s8
cstr_to_s8(char *cstr)
{
	s8 result = {.data = (u8 *)cstr};
	while (*cstr) { result.len++; cstr++; }
	return result;
}

static s8
s8_cut_head(s8 s, size cut)
{
	s8 result = s;
	if (cut > 0) {
		result.data += cut;
		result.len -= cut;
	}
	return result;
}

static s8
s8alloc(Arena *a, size len)
{
	return (s8){ .data = alloc(a, u8, len), .len = len };
}

static b32
uv4_equal(uv4 a, uv4 b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static u32
round_down_power_of_2(u32 a)
{
	u32 result = 0x80000000UL >> _lzcnt_u32(a);
	return result;
}

static v3
cross(v3 a, v3 b)
{
	v3 result = {
		.x = a.y * b.z - a.z * b.y,
		.y = a.z * b.x - a.x * b.z,
		.z = a.x * b.y - a.y * b.x,
	};
	return result;
}

static v3
sub_v3(v3 a, v3 b)
{
	v3 result = {
		.x = a.x - b.x,
		.y = a.y - b.y,
		.z = a.z - b.z,
	};
	return result;
}

static f32
length_v3(v3 a)
{
	f32 result = a.x * a.x + a.y * a.y + a.z * a.z;
	return result;
}

static v3
normalize_v3(v3 a)
{
	f32 length = length_v3(a);
	v3 result = {.x = a.x / length, .y = a.y / length, .z = a.z / length};
	return result;
}

static f64
parse_f64(s8 s)
{
	f64 integral = 0, fractional = 0, sign = 1;

	if (s.len && *s.data == '-') {
		sign = -1;
		s.data++;
		s.len--;
	}

	while (s.len && *s.data != '.') {
		integral *= 10;
		integral += *s.data - '0';
		s.data++;
		s.len--;
	}

	if (*s.data == '.') { s.data++; s.len--; }

	while (s.len) {
		ASSERT(s.data[s.len - 1] != '.');
		fractional /= 10;
		fractional += (f64)(s.data[--s.len] - '0') / 10.0;
	}
	f64 result = sign * (integral + fractional);
	return result;
}

static void
fill_hadamard(i32 *m, u32 dim)
{
	ASSERT(dim && ISPOWEROF2(dim));

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
}
