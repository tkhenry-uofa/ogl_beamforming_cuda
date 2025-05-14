/* See LICENSE for license details. */
function u32
compile_shader(OS *os, Arena a, u32 type, s8 shader, s8 name)
{
	u32 sid = glCreateShader(type);
	glShaderSource(sid, 1, (const char **)&shader.data, (int *)&shader.len);
	glCompileShader(sid);

	i32 res = 0;
	glGetShaderiv(sid, GL_COMPILE_STATUS, &res);

	if (res == GL_FALSE) {
		Stream buf = arena_stream(a);
		stream_append_s8s(&buf, name, s8(": failed to compile\n"));

		i32 len = 0, out_len = 0;
		glGetShaderiv(sid, GL_INFO_LOG_LENGTH, &len);
		glGetShaderInfoLog(sid, len, &out_len, (char *)(buf.data + buf.widx));
		stream_commit(&buf, out_len);
		glDeleteShader(sid);
		os->write_file(os->error_handle, stream_to_s8(&buf));

		sid = 0;
	}

	return sid;
}

function u32
link_program(OS *os, Arena a, u32 *shader_ids, u32 shader_id_count)
{
	i32 success = 0;
	u32 result  = glCreateProgram();
	for (u32 i = 0; i < shader_id_count; i++)
		glAttachShader(result, shader_ids[i]);
	glLinkProgram(result);
	glGetProgramiv(result, GL_LINK_STATUS, &success);
	if (success == GL_FALSE) {
		i32 len    = 0;
		Stream buf = arena_stream(a);
		stream_append_s8(&buf, s8("shader link error: "));
		glGetProgramInfoLog(result, buf.cap - buf.widx, &len, (c8 *)(buf.data + buf.widx));
		stream_reset(&buf, len);
		stream_append_byte(&buf, '\n');
		os->write_file(os->error_handle, stream_to_s8(&buf));
		glDeleteProgram(result);
		result = 0;
	}
	return result;
}

function u32
load_shader(OS *os, Arena arena, b32 compute, s8 vs_text, s8 fs_text, s8 cs_text, s8 info_name, s8 label)
{
	u32 result = 0;
	if (compute) {
		u32 shader_id = compile_shader(os, arena, GL_COMPUTE_SHADER, cs_text, info_name);
		if (shader_id) result = link_program(os, arena, (u32 []){shader_id}, 1);
		glDeleteShader(shader_id);
	} else {
		u32 fs_id = compile_shader(os, arena, GL_FRAGMENT_SHADER, fs_text, info_name);
		u32 vs_id = compile_shader(os, arena, GL_VERTEX_SHADER,   vs_text, info_name);
		if (fs_id && vs_id) result = link_program(os, arena, (u32 []){vs_id, fs_id}, 2);
		glDeleteShader(fs_id);
		glDeleteShader(vs_id);
	}

	if (result) {
		Stream buf = arena_stream(arena);
		stream_append_s8s(&buf, s8("loaded: "), info_name, s8("\n"));
		os->write_file(os->error_handle, stream_to_s8(&buf));
		LABEL_GL_OBJECT(GL_PROGRAM, result, label);
	}

	return result;
}
