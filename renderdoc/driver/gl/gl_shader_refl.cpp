/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2014 Baldur Karlsson
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "gl_shader_refl.h"

#include <algorithm>

GLuint MakeSeparableShaderProgram(const GLHookSet &gl, GLenum type, vector<string> sources)
{
	const string block = "\nout gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[]; };";

	const char **strings = new const char*[sources.size()];
	for(size_t i=0; i < sources.size(); i++)
		strings[i] = sources[i].c_str();

	GLuint sepProg = gl.glCreateShaderProgramv(type, (GLsizei)sources.size(), strings);

	GLint status;
	gl.glGetProgramiv(sepProg, eGL_LINK_STATUS, &status);

	if(status == 0 && type == eGL_VERTEX_SHADER)
	{
		gl.glDeleteProgram(sepProg);
		sepProg = 0;

		// try and patch up shader
		// naively insert gl_PerVertex block as soon as it's valid (after #version)
		// this will fail if e.g. a member of gl_PerVertex is declared at global scope
		// (this is probably most likely for clipdistance if it's redeclared with a size)

		string substituted;

		for(size_t i=0; i < sources.size(); i++)
		{
			string &src = sources[i];

			size_t len = src.length();
			size_t it = src.find("#version");
			if(it == string::npos)
				continue;

			// skip #version
			it += sizeof("#version")-1;

			// skip whitespace
			while(it < len && (src[it] == ' ' || src[it] == '\t'))
				++it;

			// skip number
			while(it < len && src[it] >= '0' && src[it] <= '9')
				++it;

			// skip whitespace
			while(it < len && (src[it] == ' ' || src[it] == '\t'))
				++it;

			substituted = src;

			if(!strncmp(&substituted[it], "core"         ,  4)) it += sizeof("core")-1;
			if(!strncmp(&substituted[it], "compatibility", 13)) it += sizeof("compatibility")-1;
			if(!strncmp(&substituted[it], "es"           ,  2)) it += sizeof("es")-1;

			substituted.insert(it, block);

			strings[i] = substituted.c_str();

			break;
		}

		sepProg = gl.glCreateShaderProgramv(type, (GLsizei)sources.size(), strings);

		gl.glGetProgramiv(sepProg, eGL_LINK_STATUS, &status);
		if(status == 0)
		{
			gl.glDeleteProgram(sepProg);
			sepProg = 0;
		}
	}

	delete[] strings;

	return sepProg;
}

void MakeShaderReflection(const GLHookSet &gl, GLenum shadType, GLuint sepProg, ShaderReflection &refl)
{
	refl.DebugInfo.entryFunc = "main";
	refl.DebugInfo.compileFlags = 0;

	refl.Disassembly = "";

	vector<ShaderResource> resources;

	GLint numUniforms = 0;
	gl.glGetProgramInterfaceiv(sepProg, eGL_UNIFORM, eGL_ACTIVE_RESOURCES, &numUniforms);

	const size_t numProps = 7;

	GLenum resProps[numProps] = {
		eGL_TYPE, eGL_NAME_LENGTH, eGL_LOCATION, eGL_BLOCK_INDEX, eGL_ARRAY_SIZE, eGL_OFFSET, eGL_IS_ROW_MAJOR,
	};
	
	for(GLint u=0; u < numUniforms; u++)
	{
		GLint values[numProps];
		gl.glGetProgramResourceiv(sepProg, eGL_UNIFORM, u, numProps, resProps, numProps, NULL, values);
		
		ShaderResource res;
		res.IsSampler = false; // no separate sampler objects in GL
		res.IsSRV = true;
		res.IsTexture = true;
		res.IsUAV = false;
		res.variableType.descriptor.rows = 1;
		res.variableType.descriptor.cols = 4;
		res.variableType.descriptor.elements = 1;
		res.variableType.descriptor.rowMajorStorage = false;
		res.bindPoint = (int32_t)resources.size();

		// float samplers
		if(values[0] == GL_SAMPLER_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "samplerBuffer";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "sampler1D";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "sampler1DArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_1D_SHADOW)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "sampler1DShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_1D_ARRAY_SHADOW)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "sampler1DArrayShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2D";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "sampler2DArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_SHADOW)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2DShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_ARRAY_SHADOW)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "sampler2DArrayShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_RECT)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2DRect";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_RECT_SHADOW)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2DRectShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "sampler3D";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "samplerCube";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_CUBE_SHADOW)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "samplerCubeShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "samplerCubeArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "sampler2DMS";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == GL_SAMPLER_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "sampler2DMSArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		// int samplers
		else if(values[0] == GL_INT_SAMPLER_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "samplerBuffer";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "sampler1D";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "sampler1DArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2D";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "sampler2DArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_2D_RECT)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2DRect";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "sampler3D";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "samplerCube";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "samplerCubeArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "sampler2DMS";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "sampler2DMSArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		// unsigned int samplers
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "samplerBuffer";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "sampler1D";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "sampler1DArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2D";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "sampler2DArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_2D_RECT)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2DRect";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "sampler3D";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "samplerCube";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "samplerCubeArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "sampler2DMS";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "sampler2DMSArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else
		{
			// not a sampler
			continue;
		}

		create_array_uninit(res.name, values[1]);
		gl.glGetProgramResourceName(sepProg, eGL_UNIFORM, u, values[1], NULL, res.name.elems);
		res.name.count--; // trim off trailing null

		resources.push_back(res);
	}

	refl.Resources = resources;

	vector<ShaderConstant> globalUniforms;
	
	GLint numUBOs = 0;
	vector<string> uboNames;
	vector<ShaderConstant> *ubos = NULL;
	
	{
		gl.glGetProgramInterfaceiv(sepProg, eGL_UNIFORM_BLOCK, eGL_ACTIVE_RESOURCES, &numUBOs);

		ubos = new vector<ShaderConstant>[numUBOs];
		uboNames.resize(numUBOs);

		for(GLint u=0; u < numUBOs; u++)
		{
			GLenum nameLen = eGL_NAME_LENGTH;
			GLint len;
			gl.glGetProgramResourceiv(sepProg, eGL_UNIFORM_BLOCK, u, 1, &nameLen, 1, NULL, &len);

			char *nm = new char[len+1];
			gl.glGetProgramResourceName(sepProg, eGL_UNIFORM_BLOCK, u, len+1, NULL, nm);
			uboNames[u] = nm;
			delete[] nm;
		}
	}

	for(GLint u=0; u < numUniforms; u++)
	{
		GLint values[numProps];
		gl.glGetProgramResourceiv(sepProg, eGL_UNIFORM, u, numProps, resProps, numProps, NULL, values);
		
		ShaderConstant var;
		
		var.type.descriptor.elements = RDCMAX(1, values[4]);

		// set type (or bail if it's not a variable - sampler or such)
		switch(values[0])
		{
			case GL_FLOAT_VEC4:
			case GL_FLOAT_VEC3:
			case GL_FLOAT_VEC2:
			case GL_FLOAT:
			case GL_FLOAT_MAT4:
			case GL_FLOAT_MAT3:
			case GL_FLOAT_MAT2:
			case GL_FLOAT_MAT4x2:
			case GL_FLOAT_MAT4x3:
			case GL_FLOAT_MAT3x4:
			case GL_FLOAT_MAT3x2:
			case GL_FLOAT_MAT2x4:
			case GL_FLOAT_MAT2x3:
				var.type.descriptor.type = eVar_Float;
				break;
			case GL_DOUBLE_VEC4:
			case GL_DOUBLE_VEC3:
			case GL_DOUBLE_VEC2:
			case GL_DOUBLE:
			case GL_DOUBLE_MAT4:
			case GL_DOUBLE_MAT3:
			case GL_DOUBLE_MAT2:
			case GL_DOUBLE_MAT4x2:
			case GL_DOUBLE_MAT4x3:
			case GL_DOUBLE_MAT3x4:
			case GL_DOUBLE_MAT3x2:
			case GL_DOUBLE_MAT2x4:
			case GL_DOUBLE_MAT2x3:
				var.type.descriptor.type = eVar_Double;
				break;
			case GL_UNSIGNED_INT_VEC4:
			case GL_UNSIGNED_INT_VEC3:
			case GL_UNSIGNED_INT_VEC2:
			case GL_UNSIGNED_INT:
			case GL_BOOL_VEC4:
			case GL_BOOL_VEC3:
			case GL_BOOL_VEC2:
			case GL_BOOL:
				var.type.descriptor.type = eVar_UInt;
				break;
			case GL_INT_VEC4:
			case GL_INT_VEC3:
			case GL_INT_VEC2:
			case GL_INT:
				var.type.descriptor.type = eVar_Int;
				break;
			default:
				// not a variable (sampler etc)
				continue;
		}
		
		// set # rows if it's a matrix
		var.type.descriptor.rows = 1;

		switch(values[0])
		{
			case GL_FLOAT_MAT4:
			case GL_DOUBLE_MAT4:
			case GL_FLOAT_MAT2x4:
			case GL_DOUBLE_MAT2x4:
			case GL_FLOAT_MAT3x4:
			case GL_DOUBLE_MAT3x4:
				var.type.descriptor.rows = 4;
				break;
			case GL_FLOAT_MAT3:
			case GL_DOUBLE_MAT3:
			case GL_FLOAT_MAT4x3:
			case GL_DOUBLE_MAT4x3:
			case GL_FLOAT_MAT2x3:
			case GL_DOUBLE_MAT2x3:
				var.type.descriptor.rows = 3;
				break;
			case GL_FLOAT_MAT2:
			case GL_DOUBLE_MAT2:
			case GL_FLOAT_MAT4x2:
			case GL_DOUBLE_MAT4x2:
			case GL_FLOAT_MAT3x2:
			case GL_DOUBLE_MAT3x2:
				var.type.descriptor.rows = 2;
				break;
			default:
				break;
		}

		// set # columns
		switch(values[0])
		{
			case GL_FLOAT_VEC4:
			case GL_FLOAT_MAT4:
			case GL_FLOAT_MAT4x2:
			case GL_FLOAT_MAT4x3:
			case GL_DOUBLE_VEC4:
			case GL_DOUBLE_MAT4:
			case GL_DOUBLE_MAT4x2:
			case GL_DOUBLE_MAT4x3:
			case GL_UNSIGNED_INT_VEC4:
			case GL_BOOL_VEC4:
			case GL_INT_VEC4:
				var.type.descriptor.cols = 4;
				break;
			case GL_FLOAT_VEC3:
			case GL_FLOAT_MAT3:
			case GL_FLOAT_MAT3x4:
			case GL_FLOAT_MAT3x2:
			case GL_DOUBLE_VEC3:
			case GL_DOUBLE_MAT3:
			case GL_DOUBLE_MAT3x4:
			case GL_DOUBLE_MAT3x2:
			case GL_UNSIGNED_INT_VEC3:
			case GL_BOOL_VEC3:
			case GL_INT_VEC3:
				var.type.descriptor.cols = 3;
				break;
			case GL_FLOAT_VEC2:
			case GL_FLOAT_MAT2:
			case GL_FLOAT_MAT2x4:
			case GL_FLOAT_MAT2x3:
			case GL_DOUBLE_VEC2:
			case GL_DOUBLE_MAT2:
			case GL_DOUBLE_MAT2x4:
			case GL_DOUBLE_MAT2x3:
			case GL_UNSIGNED_INT_VEC2:
			case GL_BOOL_VEC2:
			case GL_INT_VEC2:
				var.type.descriptor.cols = 2;
				break;
			case GL_FLOAT:
			case GL_DOUBLE:
			case GL_UNSIGNED_INT:
			case GL_INT:
			case GL_BOOL:
				var.type.descriptor.cols = 1;
				break;
			default:
				break;
		}

		// set name
		switch(values[0])
		{
			case GL_FLOAT_VEC4:          var.type.descriptor.name = "vec4"; break;
			case GL_FLOAT_VEC3:          var.type.descriptor.name = "vec3"; break;
			case GL_FLOAT_VEC2:          var.type.descriptor.name = "vec2"; break;
			case GL_FLOAT:               var.type.descriptor.name = "float"; break;
			case GL_FLOAT_MAT4:          var.type.descriptor.name = "mat4"; break;
			case GL_FLOAT_MAT3:          var.type.descriptor.name = "mat3"; break;
			case GL_FLOAT_MAT2:          var.type.descriptor.name = "mat2"; break;
			case GL_FLOAT_MAT4x2:        var.type.descriptor.name = "mat4x2"; break;
			case GL_FLOAT_MAT4x3:        var.type.descriptor.name = "mat4x3"; break;
			case GL_FLOAT_MAT3x4:        var.type.descriptor.name = "mat3x4"; break;
			case GL_FLOAT_MAT3x2:        var.type.descriptor.name = "mat3x2"; break;
			case GL_FLOAT_MAT2x4:        var.type.descriptor.name = "mat2x4"; break;
			case GL_FLOAT_MAT2x3:        var.type.descriptor.name = "mat2x3"; break;
			case GL_DOUBLE_VEC4:         var.type.descriptor.name = "dvec4"; break;
			case GL_DOUBLE_VEC3:         var.type.descriptor.name = "dvec3"; break;
			case GL_DOUBLE_VEC2:         var.type.descriptor.name = "dvec2"; break;
			case GL_DOUBLE:              var.type.descriptor.name = "double"; break;
			case GL_DOUBLE_MAT4:         var.type.descriptor.name = "dmat4"; break;
			case GL_DOUBLE_MAT3:         var.type.descriptor.name = "dmat3"; break;
			case GL_DOUBLE_MAT2:         var.type.descriptor.name = "dmat2"; break;
			case GL_DOUBLE_MAT4x2:       var.type.descriptor.name = "dmat4x2"; break;
			case GL_DOUBLE_MAT4x3:       var.type.descriptor.name = "dmat4x3"; break;
			case GL_DOUBLE_MAT3x4:       var.type.descriptor.name = "dmat3x4"; break;
			case GL_DOUBLE_MAT3x2:       var.type.descriptor.name = "dmat3x2"; break;
			case GL_DOUBLE_MAT2x4:       var.type.descriptor.name = "dmat2x4"; break;
			case GL_DOUBLE_MAT2x3:       var.type.descriptor.name = "dmat2x3"; break;
			case GL_UNSIGNED_INT_VEC4:   var.type.descriptor.name = "uvec4"; break;
			case GL_UNSIGNED_INT_VEC3:   var.type.descriptor.name = "uvec3"; break;
			case GL_UNSIGNED_INT_VEC2:   var.type.descriptor.name = "uvec2"; break;
			case GL_UNSIGNED_INT:        var.type.descriptor.name = "uint"; break;
			case GL_BOOL_VEC4:           var.type.descriptor.name = "bvec4"; break;
			case GL_BOOL_VEC3:           var.type.descriptor.name = "bvec3"; break;
			case GL_BOOL_VEC2:           var.type.descriptor.name = "bvec2"; break;
			case GL_BOOL:                var.type.descriptor.name = "bool"; break;
			case GL_INT_VEC4:            var.type.descriptor.name = "ivec4"; break;
			case GL_INT_VEC3:            var.type.descriptor.name = "ivec3"; break;
			case GL_INT_VEC2:            var.type.descriptor.name = "ivec2"; break;
			case GL_INT:                 var.type.descriptor.name = "int"; break;
			default:
				break;
		}

		if(values[5] == -1 && values[2] >= 0)
		{
			var.reg.vec = values[2];
			var.reg.comp = 0;
		}
		else if(values[5] >= 0)
		{
			var.reg.vec = values[5] / 16;
			var.reg.comp = (values[5] / 4) % 4;

			RDCASSERT((values[5] % 4) == 0);
		}
		else
		{
			var.reg.vec = var.reg.comp = ~0U;
		}

		var.type.descriptor.rowMajorStorage = (values[6] >= 0);

		create_array_uninit(var.name, values[1]);
		gl.glGetProgramResourceName(sepProg, eGL_UNIFORM, u, values[0], NULL, var.name.elems);
		var.name.count--; // trim off trailing null

		int32_t c = var.name.count;

		// trim off trailing [0] if it's an array
		if(values[4] > 1 && var.name[c-3] == '[' && var.name[c-2] == '0' && var.name[c-1] == ']')
			var.name.count -= 3;

		if(strchr(var.name.elems, '.'))
		{
			GLNOTIMP("Variable contains . - structure not reconstructed");
		}
		
		vector<ShaderConstant> *UBO = &globalUniforms;

		// don't look at block uniforms just yet
		if(values[3] != -1)
		{
			RDCASSERT(values[3] < numUBOs);
			UBO = &ubos[ values[3] ];
		}
		
		UBO->push_back(var);
	}

	vector<ConstantBlock> cbuffers;
	
	if(ubos)
	{
		cbuffers.reserve(numUBOs + (globalUniforms.empty() ? 0 : 1));

		for(int i=0; i < numUBOs; i++)
		{
			if(!ubos[i].empty())
			{
				struct ubo_offset_sort
				{
					bool operator() (const ShaderConstant &a, const ShaderConstant &b)
					{ if(a.reg.vec == b.reg.vec) return a.reg.comp < b.reg.comp; else return a.reg.vec < b.reg.vec; }
				};

				ConstantBlock cblock;
				cblock.name = uboNames[i];
				cblock.bufferBacked = true;
				cblock.bindPoint = (int32_t)cbuffers.size();
				std::sort(ubos[i].begin(), ubos[i].end(), ubo_offset_sort());

				cblock.variables = ubos[i];

				cbuffers.push_back(cblock);
			}
		}
	}

	if(!globalUniforms.empty())
	{
		ConstantBlock globals;
		globals.name = "$Globals";
		globals.bufferBacked = false;
		globals.bindPoint = (int32_t)cbuffers.size();
		globals.variables = globalUniforms;

		cbuffers.push_back(globals);
	}

	delete[] ubos;
	for(int sigType=0; sigType < 2; sigType++)
	{
		GLenum sigEnum = (sigType == 0 ? eGL_PROGRAM_INPUT : eGL_PROGRAM_OUTPUT);
		rdctype::array<SigParameter> *sigArray = (sigType == 0 ? &refl.InputSig : &refl.OutputSig);

		GLint numInputs;
		gl.glGetProgramInterfaceiv(sepProg, sigEnum, eGL_ACTIVE_RESOURCES, &numInputs);
		
		if(numInputs > 0)
		{
			vector<SigParameter> sigs;
			sigs.reserve(numInputs);
			for(GLint i=0; i < numInputs; i++)
			{
				GLenum props[] = { eGL_NAME_LENGTH, eGL_TYPE, eGL_LOCATION, eGL_LOCATION_COMPONENT };
				GLint values[] = { 0              , 0       , 0           , 0                      };
				gl.glGetProgramResourceiv(sepProg, sigEnum, i, ARRAY_COUNT(props), props, ARRAY_COUNT(props), NULL, values);

				char *nm = new char[values[0]+1];
				gl.glGetProgramResourceName(sepProg, sigEnum, i, values[0]+1, NULL, nm);
				
				SigParameter sig;

				sig.varName = nm;
				sig.semanticIndex = 0;
				sig.needSemanticIndex = false;
				sig.stream = 0;
				
				switch(values[1])
				{
					case GL_FLOAT:
					case GL_DOUBLE:
					case GL_INT:
					case GL_UNSIGNED_INT:
					case GL_BOOL:
						sig.compCount = 1;
						sig.regChannelMask = 0x1;
						break;
					case GL_FLOAT_VEC2:
					case GL_DOUBLE_VEC2:
					case GL_INT_VEC2:
					case GL_UNSIGNED_INT_VEC2:
					case GL_BOOL_VEC2:
						sig.compCount = 2;
						sig.regChannelMask = 0x3;
						break;
					case GL_FLOAT_VEC3:
					case GL_DOUBLE_VEC3:
					case GL_INT_VEC3:
					case GL_UNSIGNED_INT_VEC3:
					case GL_BOOL_VEC3:
						sig.compCount = 3;
						sig.regChannelMask = 0x7;
						break;
					case GL_FLOAT_VEC4:
					case GL_DOUBLE_VEC4:
					case GL_INT_VEC4:
					case GL_UNSIGNED_INT_VEC4:
					case GL_BOOL_VEC4:
						sig.compCount = 4;
						sig.regChannelMask = 0xf;
						break;
					default:
						RDCWARN("Unhandled signature element type %x", values[1]);
						sig.compCount = 4;
						sig.regChannelMask = 0xf;
						break;
				}
			
				sig.regChannelMask <<= values[3];

				sig.channelUsedMask = sig.regChannelMask;

				sig.systemValue = eAttr_None;

#define IS_BUITIN(builtin) !strncmp(nm, builtin, sizeof(builtin)-1)

				// VS built-in inputs
				if(IS_BUITIN("gl_VertexID"))             sig.systemValue = eAttr_VertexIndex;
				if(IS_BUITIN("gl_InstanceID"))           sig.systemValue = eAttr_InstanceIndex;

				// VS built-in outputs
				if(IS_BUITIN("gl_Position"))             sig.systemValue = eAttr_Position;
				if(IS_BUITIN("gl_PointSize"))            sig.systemValue = eAttr_PointSize;
				if(IS_BUITIN("gl_ClipDistance"))         sig.systemValue = eAttr_ClipDistance;
				
				// TCS built-in inputs
				if(IS_BUITIN("gl_PatchVerticesIn"))      sig.systemValue = eAttr_PatchNumVertices;
				if(IS_BUITIN("gl_PrimitiveID"))          sig.systemValue = eAttr_PrimitiveIndex;
				if(IS_BUITIN("gl_InvocationID"))         sig.systemValue = eAttr_InvocationIndex;

				// TCS built-in outputs
				if(IS_BUITIN("gl_TessLevelOuter"))       sig.systemValue = eAttr_OuterTessFactor;
				if(IS_BUITIN("gl_TessLevelInner"))       sig.systemValue = eAttr_InsideTessFactor;
				
				// TES built-in inputs
				if(IS_BUITIN("gl_TessCoord"))            sig.systemValue = eAttr_DomainLocation;
				if(IS_BUITIN("gl_PatchVerticesIn"))      sig.systemValue = eAttr_PatchNumVertices;
				if(IS_BUITIN("gl_PrimitiveID"))          sig.systemValue = eAttr_PrimitiveIndex;
				
				// GS built-in inputs
				if(IS_BUITIN("gl_PrimitiveIDIn"))        sig.systemValue = eAttr_PrimitiveIndex;
				if(IS_BUITIN("gl_InvocationID"))         sig.systemValue = eAttr_InvocationIndex;
				if(IS_BUITIN("gl_Layer"))                sig.systemValue = eAttr_RTIndex;
				if(IS_BUITIN("gl_ViewportIndex"))        sig.systemValue = eAttr_ViewportIndex;

				// GS built-in outputs
				if(IS_BUITIN("gl_Layer"))                sig.systemValue = eAttr_RTIndex;
				if(IS_BUITIN("gl_ViewportIndex"))        sig.systemValue = eAttr_ViewportIndex;
				
				// PS built-in inputs
				if(IS_BUITIN("gl_FragCoord"))            sig.systemValue = eAttr_Position;
				if(IS_BUITIN("gl_FrontFacing"))          sig.systemValue = eAttr_IsFrontFace;
				if(IS_BUITIN("gl_PointCoord"))           sig.systemValue = eAttr_RTIndex;
				if(IS_BUITIN("gl_SampleID"))             sig.systemValue = eAttr_MSAASampleIndex;
				if(IS_BUITIN("gl_SamplePosition"))       sig.systemValue = eAttr_MSAASamplePosition;
				if(IS_BUITIN("gl_SampleMaskIn"))         sig.systemValue = eAttr_MSAACoverage;
				
				// PS built-in outputs
				if(IS_BUITIN("gl_FragDepth"))            sig.systemValue = eAttr_DepthOutput;
				if(IS_BUITIN("gl_SampleMask"))           sig.systemValue = eAttr_MSAACoverage;
				
				// CS built-in inputs
				if(IS_BUITIN("gl_NumWorkGroups"))        sig.systemValue = eAttr_DispatchSize;
				if(IS_BUITIN("gl_WorkGroupID"))          sig.systemValue = eAttr_GroupIndex;
				if(IS_BUITIN("gl_LocalInvocationID"))    sig.systemValue = eAttr_GroupThreadIndex;
				if(IS_BUITIN("gl_GlobalInvocationID"))   sig.systemValue = eAttr_DispatchThreadIndex;
				if(IS_BUITIN("gl_LocalInvocationIndex")) sig.systemValue = eAttr_GroupFlatIndex;

#undef IS_BUITIN
				if(shadType == eGL_FRAGMENT_SHADER && sigEnum == eGL_PROGRAM_OUTPUT && sig.systemValue == eAttr_None)
					sig.systemValue = eAttr_ColourOutput;
				
				if(sig.systemValue == eAttr_None)
					sig.regIndex = values[2] >= 0 ? values[2] : i;
				else
					sig.regIndex = values[2] >= 0 ? values[2] : 0;

				delete[] nm;

				sigs.push_back(sig);
			}
			struct sig_param_sort
			{
				bool operator() (const SigParameter &a, const SigParameter &b)
				{ if(a.systemValue == b.systemValue) return a.regIndex < b.regIndex; return a.systemValue < b.systemValue; }
			};
			
			std::sort(sigs.begin(), sigs.end(), sig_param_sort());
			
			*sigArray = sigs;
		}
	}
	
	// TODO: fill in Interfaces with shader subroutines?

	refl.ConstantBlocks = cbuffers;
}
