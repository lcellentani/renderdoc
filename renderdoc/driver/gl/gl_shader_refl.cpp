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

#undef max
#undef min

#include "glslang/SPIRV/GlslangToSpv.h"
#include "glslang/glslang/Public/ShaderLang.h"

#include "glslang/SPIRV/spirv.h"
#include "glslang/SPIRV/GLSL450Lib.h"

// declare versions of ShaderConstant/ShaderVariableType with vectors
// to more easily build up the members of nested structures
struct DynShaderConstant;

struct DynShaderVariableType
{
	struct
	{
		VarType       type;
		uint32_t      rows;
		uint32_t      cols;
		uint32_t      elements;
		bool32        rowMajorStorage;
		string        name;
	} descriptor;

	vector<DynShaderConstant> members;
};

struct DynShaderConstant
{
	string name;
	struct
	{
		uint32_t vec;
		uint32_t comp;
	} reg;
	DynShaderVariableType type;
};

void sort(vector<DynShaderConstant> &vars)
{
	if(vars.empty()) return;

	struct offset_sort
	{
		bool operator() (const DynShaderConstant &a, const DynShaderConstant &b)
		{ if(a.reg.vec == b.reg.vec) return a.reg.comp < b.reg.comp; else return a.reg.vec < b.reg.vec; }
	};

	std::sort(vars.begin(), vars.end(), offset_sort());

	for(size_t i=0; i < vars.size(); i++)
		sort(vars[i].type.members);
}

void copy(rdctype::array<ShaderConstant> &outvars, const vector<DynShaderConstant> &invars)
{
	if(invars.empty())
	{
		RDCEraseEl(outvars);
		return;
	}

	create_array_uninit(outvars, invars.size());
	for(size_t i=0; i < invars.size(); i++)
	{
		outvars[i].name = invars[i].name;
		outvars[i].reg.vec = invars[i].reg.vec;
		outvars[i].reg.comp = invars[i].reg.comp;
		outvars[i].type.descriptor.type = invars[i].type.descriptor.type;
		outvars[i].type.descriptor.rows = invars[i].type.descriptor.rows;
		outvars[i].type.descriptor.cols = invars[i].type.descriptor.cols;
		outvars[i].type.descriptor.elements = invars[i].type.descriptor.elements;
		outvars[i].type.descriptor.rowMajorStorage = invars[i].type.descriptor.rowMajorStorage;
		outvars[i].type.descriptor.name = invars[i].type.descriptor.name;
		copy(outvars[i].type.members, invars[i].type.members);
	}
}

void CheckVertexOutputUses(const vector<string> &sources, bool &pointSizeUsed, bool &clipDistanceUsed)
{
	pointSizeUsed = false;
	clipDistanceUsed = false;

	for(size_t i=0; i < sources.size(); i++)
	{
		const string &s = sources[i];

		size_t offs = 0;

		while(true)
		{
			offs = s.find("gl_PointSize", offs);

			if(offs == string::npos)
				break;

			// consider gl_PointSize used if we encounter a '=' before a ';' or the end of the string

			while(offs < s.length())
			{
				if(s[offs] == '=')
				{
					pointSizeUsed = true;
					break;
				}

				if(s[offs] == ';')
					break;

				offs++;
			}
		}

		offs = 0;

		while(true)
		{
			offs = s.find("gl_ClipDistance", offs);

			if(offs == string::npos)
				break;

			// consider gl_ClipDistance used if we encounter a '=' before a ';' or the end of the string

			while(offs < s.length())
			{
				if(s[offs] == '=')
				{
					clipDistanceUsed = true;
					break;
				}

				if(s[offs] == ';')
					break;

				offs++;
			}
		}
	}
}

// little utility function that if necessary emulates glCreateShaderProgramv functionality but using glCompileShaderIncludeARB
static GLuint CreateSepProgram(const GLHookSet &gl, GLenum type, GLsizei numSources, const char **sources, GLsizei numPaths, const char **paths)
{
	// definition of glCreateShaderProgramv from the spec
	GLuint shader = gl.glCreateShader(type);
	if(shader)
	{
		gl.glShaderSource(shader, numSources, sources, NULL);

		if(paths == NULL)
			gl.glCompileShader(shader);
		else
			gl.glCompileShaderIncludeARB(shader, numPaths, paths, NULL);

		GLuint program = gl.glCreateProgram();
		if(program)
		{
			GLint compiled = 0;

			gl.glGetShaderiv(shader, eGL_COMPILE_STATUS, &compiled);
			gl.glProgramParameteri(program, eGL_PROGRAM_SEPARABLE, GL_TRUE);

			if(compiled)
			{
				gl.glAttachShader(program, shader);
				gl.glLinkProgram(program);

				// we deliberately leave the shaders attached so this program can be re-linked.
				// they will be cleaned up when the program is deleted
				// gl.glDetachShader(program, shader);
			}
		}
		gl.glDeleteShader(shader);
		return program;
	}

	return 0;
}

GLuint MakeSeparableShaderProgram(const GLHookSet &gl, GLenum type, vector<string> sources, vector<string> *includepaths)
{
	// in and out blocks are added separately, in case one is there already
	const char *blockIdentifiers[2] = { "in gl_PerVertex", "out gl_PerVertex" };
	string blocks[2] = { "", "" };
	
	if(type == eGL_VERTEX_SHADER)
	{
		blocks[1] = "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[]; };\n";
	}
	else if(type == eGL_TESS_CONTROL_SHADER)
	{
		blocks[0] = "in gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[]; } gl_in[];\n";
		blocks[1] = "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[]; } gl_out[];\n";
	}
	else
	{
		blocks[0] = "in gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[]; } gl_in[];\n";
		blocks[1] = "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[]; };\n";
	}

	const char **strings = new const char*[sources.size()];
	for(size_t i=0; i < sources.size(); i++)
		strings[i] = sources[i].c_str();
	
	const char **paths = NULL;
	GLsizei numPaths = 0;
	if(includepaths)
	{
		numPaths = (GLsizei)includepaths->size();

		paths = new const char*[includepaths->size()];
		for(size_t i=0; i < includepaths->size(); i++)
			paths[i] = (*includepaths)[i].c_str();
	}

	GLuint sepProg = CreateSepProgram(gl, type, (GLsizei)sources.size(), strings, numPaths, paths);

	GLint status;
	gl.glGetProgramiv(sepProg, eGL_LINK_STATUS, &status);

	// allow any vertex processing shader to redeclare gl_PerVertex
	if(status == 0 && type != eGL_FRAGMENT_SHADER && type != eGL_COMPUTE_SHADER)
	{
		gl.glDeleteProgram(sepProg);
		sepProg = 0;

		// try and patch up shader
		// naively insert gl_PerVertex block as soon as it's valid (after #version)
		// this will fail if e.g. a member of gl_PerVertex is declared at global scope
		// (this is probably most likely for clipdistance if it's redeclared with a size)

		// these strings contain whichever source string we replaced, here to scope until
		// the program has been created
		string subStrings[2];

		for(int blocktype = 0; blocktype < 2; blocktype++)
		{
			// vertex shaders don't have an in block
			if(type == eGL_VERTEX_SHADER && blocktype == 0) continue;

			string &substituted = subStrings[blocktype];

			string block = blocks[blocktype];
			const char *identifier = blockIdentifiers[blocktype];

			bool already = false;

			for(size_t i=0; i < sources.size(); i++)
			{
				// if we find the 'identifier' (ie. the block name),
				// assume this block is already present and stop
				if(sources[i].find(identifier) != string::npos)
				{
					already = true;
					break;
				}
			}

			// only try and insert this block if the shader doesn't already have it
			if(already) continue;

			for(size_t i=0; i < sources.size(); i++)
			{
				string src = strings[i];

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

				if(!strncmp(&src[it], "core"         ,  4)) it += sizeof("core")-1;
				if(!strncmp(&src[it], "compatibility", 13)) it += sizeof("compatibility")-1;
				if(!strncmp(&src[it], "es"           ,  2)) it += sizeof("es")-1;

				// now skip past comments, and any #extension directives
				while(it < len)
				{
					// skip whitespace
					while(it < len && (src[it] == ' ' || src[it] == '\t' || src[it] == '\r' || src[it] == '\n'))
						++it;

					// skip C++ style comments
					if(it+1 < len && src[it] == '/' && src[it+1] == '/')
					{
						// keep going until the next newline
						while(it < len && src[it] != '\r' && src[it] != '\n')
							++it;

						// skip more things
						continue;
					}

					// skip extension directives
					const char extDirective[] = "#extension";
					if(!strncmp(src.c_str()+it, extDirective, sizeof(extDirective)-1) &&
						it+sizeof(extDirective)-1 < len &&
						(src[it+sizeof(extDirective)-1] == ' ' || src[it+sizeof(extDirective)-1] == '\t'))
					{
						// keep going until the next newline
						while(it < len && src[it] != '\r' && src[it] != '\n')
							++it;

						// skip more things
						continue;
					}

					// skip C style comments
					if(it+1 < len && src[it] == '/' && src[it+1] == '*')
					{
						// keep going until the we reach a */
						while(it+1 < len && (src[it] != '*' || src[it+1] != '/'))
							++it;

						// skip more things
						continue;
					}

					// nothing more to skip
					break;
				}

				substituted = src;

				substituted.insert(it, block);

				strings[i] = substituted.c_str();

				break;
			}
		}

		sepProg = CreateSepProgram(gl, type, (GLsizei)sources.size(), strings, numPaths, paths);
	}

	gl.glGetProgramiv(sepProg, eGL_LINK_STATUS, &status);
	if(status == 0)
	{
		char buffer[1025] = {0};
		gl.glGetProgramInfoLog(sepProg, 1024, NULL, buffer);

		RDCERR("Couldn't make separable shader program for shader. Errors:\n%s", buffer);

		gl.glDeleteProgram(sepProg);
		sepProg = 0;
	}

	delete[] strings;
	if(paths) delete[] paths;

	return sepProg;
}

void ReconstructVarTree(const GLHookSet &gl, GLenum query, GLuint sepProg, GLuint varIdx,
	GLint numParentBlocks, vector<DynShaderConstant> *parentBlocks,
	vector<DynShaderConstant> *defaultBlock)
{
	const size_t numProps = 7;

	GLenum resProps[numProps] = {
		eGL_TYPE, eGL_NAME_LENGTH, eGL_LOCATION, eGL_BLOCK_INDEX, eGL_ARRAY_SIZE, eGL_OFFSET, eGL_IS_ROW_MAJOR,
	};

	// GL_LOCATION not valid for buffer variables (it's only used if offset comes back -1, which will never
	// happen for buffer variables)
	if(query == eGL_BUFFER_VARIABLE)
		resProps[2] = eGL_OFFSET;
	
	GLint values[numProps] = { -1, -1, -1, -1, -1, -1, -1 };
	gl.glGetProgramResourceiv(sepProg, query, varIdx, numProps, resProps, numProps, NULL, values);

	DynShaderConstant var;

	var.type.descriptor.elements = RDCMAX(1, values[4]);

	// set type (or bail if it's not a variable - sampler or such)
	switch(values[0])
	{
		case eGL_FLOAT_VEC4:
		case eGL_FLOAT_VEC3:
		case eGL_FLOAT_VEC2:
		case eGL_FLOAT:
		case eGL_FLOAT_MAT4:
		case eGL_FLOAT_MAT3:
		case eGL_FLOAT_MAT2:
		case eGL_FLOAT_MAT4x2:
		case eGL_FLOAT_MAT4x3:
		case eGL_FLOAT_MAT3x4:
		case eGL_FLOAT_MAT3x2:
		case eGL_FLOAT_MAT2x4:
		case eGL_FLOAT_MAT2x3:
			var.type.descriptor.type = eVar_Float;
			break;
		case eGL_DOUBLE_VEC4:
		case eGL_DOUBLE_VEC3:
		case eGL_DOUBLE_VEC2:
		case eGL_DOUBLE:
		case eGL_DOUBLE_MAT4:
		case eGL_DOUBLE_MAT3:
		case eGL_DOUBLE_MAT2:
		case eGL_DOUBLE_MAT4x2:
		case eGL_DOUBLE_MAT4x3:
		case eGL_DOUBLE_MAT3x4:
		case eGL_DOUBLE_MAT3x2:
		case eGL_DOUBLE_MAT2x4:
		case eGL_DOUBLE_MAT2x3:
			var.type.descriptor.type = eVar_Double;
			break;
		case eGL_UNSIGNED_INT_VEC4:
		case eGL_UNSIGNED_INT_VEC3:
		case eGL_UNSIGNED_INT_VEC2:
		case eGL_UNSIGNED_INT:
		case eGL_BOOL_VEC4:
		case eGL_BOOL_VEC3:
		case eGL_BOOL_VEC2:
		case eGL_BOOL:
			var.type.descriptor.type = eVar_UInt;
			break;
		case eGL_INT_VEC4:
		case eGL_INT_VEC3:
		case eGL_INT_VEC2:
		case eGL_INT:
			var.type.descriptor.type = eVar_Int;
			break;
		default:
			// not a variable (sampler etc)
			return;
	}

	// set # rows if it's a matrix
	var.type.descriptor.rows = 1;

	switch(values[0])
	{
		case eGL_FLOAT_MAT4:
		case eGL_DOUBLE_MAT4:
		case eGL_FLOAT_MAT2x4:
		case eGL_DOUBLE_MAT2x4:
		case eGL_FLOAT_MAT3x4:
		case eGL_DOUBLE_MAT3x4:
			var.type.descriptor.rows = 4;
			break;
		case eGL_FLOAT_MAT3:
		case eGL_DOUBLE_MAT3:
		case eGL_FLOAT_MAT4x3:
		case eGL_DOUBLE_MAT4x3:
		case eGL_FLOAT_MAT2x3:
		case eGL_DOUBLE_MAT2x3:
			var.type.descriptor.rows = 3;
			break;
		case eGL_FLOAT_MAT2:
		case eGL_DOUBLE_MAT2:
		case eGL_FLOAT_MAT4x2:
		case eGL_DOUBLE_MAT4x2:
		case eGL_FLOAT_MAT3x2:
		case eGL_DOUBLE_MAT3x2:
			var.type.descriptor.rows = 2;
			break;
		default:
			break;
	}

	// set # columns
	switch(values[0])
	{
		case eGL_FLOAT_VEC4:
		case eGL_FLOAT_MAT4:
		case eGL_FLOAT_MAT4x2:
		case eGL_FLOAT_MAT4x3:
		case eGL_DOUBLE_VEC4:
		case eGL_DOUBLE_MAT4:
		case eGL_DOUBLE_MAT4x2:
		case eGL_DOUBLE_MAT4x3:
		case eGL_UNSIGNED_INT_VEC4:
		case eGL_BOOL_VEC4:
		case eGL_INT_VEC4:
			var.type.descriptor.cols = 4;
			break;
		case eGL_FLOAT_VEC3:
		case eGL_FLOAT_MAT3:
		case eGL_FLOAT_MAT3x4:
		case eGL_FLOAT_MAT3x2:
		case eGL_DOUBLE_VEC3:
		case eGL_DOUBLE_MAT3:
		case eGL_DOUBLE_MAT3x4:
		case eGL_DOUBLE_MAT3x2:
		case eGL_UNSIGNED_INT_VEC3:
		case eGL_BOOL_VEC3:
		case eGL_INT_VEC3:
			var.type.descriptor.cols = 3;
			break;
		case eGL_FLOAT_VEC2:
		case eGL_FLOAT_MAT2:
		case eGL_FLOAT_MAT2x4:
		case eGL_FLOAT_MAT2x3:
		case eGL_DOUBLE_VEC2:
		case eGL_DOUBLE_MAT2:
		case eGL_DOUBLE_MAT2x4:
		case eGL_DOUBLE_MAT2x3:
		case eGL_UNSIGNED_INT_VEC2:
		case eGL_BOOL_VEC2:
		case eGL_INT_VEC2:
			var.type.descriptor.cols = 2;
			break;
		case eGL_FLOAT:
		case eGL_DOUBLE:
		case eGL_UNSIGNED_INT:
		case eGL_INT:
		case eGL_BOOL:
			var.type.descriptor.cols = 1;
			break;
		default:
			break;
	}

	// set name
	switch(values[0])
	{
		case eGL_FLOAT_VEC4:          var.type.descriptor.name = "vec4"; break;
		case eGL_FLOAT_VEC3:          var.type.descriptor.name = "vec3"; break;
		case eGL_FLOAT_VEC2:          var.type.descriptor.name = "vec2"; break;
		case eGL_FLOAT:               var.type.descriptor.name = "float"; break;
		case eGL_FLOAT_MAT4:          var.type.descriptor.name = "mat4"; break;
		case eGL_FLOAT_MAT3:          var.type.descriptor.name = "mat3"; break;
		case eGL_FLOAT_MAT2:          var.type.descriptor.name = "mat2"; break;
		case eGL_FLOAT_MAT4x2:        var.type.descriptor.name = "mat4x2"; break;
		case eGL_FLOAT_MAT4x3:        var.type.descriptor.name = "mat4x3"; break;
		case eGL_FLOAT_MAT3x4:        var.type.descriptor.name = "mat3x4"; break;
		case eGL_FLOAT_MAT3x2:        var.type.descriptor.name = "mat3x2"; break;
		case eGL_FLOAT_MAT2x4:        var.type.descriptor.name = "mat2x4"; break;
		case eGL_FLOAT_MAT2x3:        var.type.descriptor.name = "mat2x3"; break;
		case eGL_DOUBLE_VEC4:         var.type.descriptor.name = "dvec4"; break;
		case eGL_DOUBLE_VEC3:         var.type.descriptor.name = "dvec3"; break;
		case eGL_DOUBLE_VEC2:         var.type.descriptor.name = "dvec2"; break;
		case eGL_DOUBLE:              var.type.descriptor.name = "double"; break;
		case eGL_DOUBLE_MAT4:         var.type.descriptor.name = "dmat4"; break;
		case eGL_DOUBLE_MAT3:         var.type.descriptor.name = "dmat3"; break;
		case eGL_DOUBLE_MAT2:         var.type.descriptor.name = "dmat2"; break;
		case eGL_DOUBLE_MAT4x2:       var.type.descriptor.name = "dmat4x2"; break;
		case eGL_DOUBLE_MAT4x3:       var.type.descriptor.name = "dmat4x3"; break;
		case eGL_DOUBLE_MAT3x4:       var.type.descriptor.name = "dmat3x4"; break;
		case eGL_DOUBLE_MAT3x2:       var.type.descriptor.name = "dmat3x2"; break;
		case eGL_DOUBLE_MAT2x4:       var.type.descriptor.name = "dmat2x4"; break;
		case eGL_DOUBLE_MAT2x3:       var.type.descriptor.name = "dmat2x3"; break;
		case eGL_UNSIGNED_INT_VEC4:   var.type.descriptor.name = "uvec4"; break;
		case eGL_UNSIGNED_INT_VEC3:   var.type.descriptor.name = "uvec3"; break;
		case eGL_UNSIGNED_INT_VEC2:   var.type.descriptor.name = "uvec2"; break;
		case eGL_UNSIGNED_INT:        var.type.descriptor.name = "uint"; break;
		case eGL_BOOL_VEC4:           var.type.descriptor.name = "bvec4"; break;
		case eGL_BOOL_VEC3:           var.type.descriptor.name = "bvec3"; break;
		case eGL_BOOL_VEC2:           var.type.descriptor.name = "bvec2"; break;
		case eGL_BOOL:                var.type.descriptor.name = "bool"; break;
		case eGL_INT_VEC4:            var.type.descriptor.name = "ivec4"; break;
		case eGL_INT_VEC3:            var.type.descriptor.name = "ivec3"; break;
		case eGL_INT_VEC2:            var.type.descriptor.name = "ivec2"; break;
		case eGL_INT:                 var.type.descriptor.name = "int"; break;
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

	var.type.descriptor.rowMajorStorage = (values[6] > 0);

	var.name.resize(values[1]-1);
	gl.glGetProgramResourceName(sepProg, query, varIdx, values[1], NULL, &var.name[0]);

	int32_t c = values[1]-1;

	// trim off trailing [0] if it's an array
	if(var.name[c-3] == '[' && var.name[c-2] == '0' && var.name[c-1] == ']')
		var.name.resize(c-3);
	else
		var.type.descriptor.elements = 0;

	vector<DynShaderConstant> *parentmembers = defaultBlock;

	if(values[3] != -1 && values[3] < numParentBlocks)
	{
		parentmembers = &parentBlocks[ values[3] ];
	}

	if(parentmembers == NULL)
	{
		RDCWARN("Found variable '%s' without parent block index '%d'", var.name.c_str(), values[3]);
		return;
	}

	char *nm = &var.name[0];

	// reverse figure out structures and structure arrays
	while(strchr(nm, '.') || strchr(nm, '['))
	{
		char *base = nm;
		while(*nm != '.' && *nm != '[') nm++;

		// determine if we have an array index, and NULL out
		// what's after the base variable name
		bool isarray = (*nm == '[');
		*nm = 0; nm++;

		int arrayIdx = 0;

		// if it's an array, get the index used
		if(isarray)
		{
			// get array index, it's always a decimal number
			while(*nm >= '0' && *nm <= '9')
			{
				arrayIdx *= 10;
				arrayIdx += int(*nm) - int('0');
				nm++;
			}

			RDCASSERT(*nm == ']');
			*nm = 0; nm++;

			// skip forward to the child name
			if(*nm == '.')
			{
				*nm = 0; nm++;
			}
			else
			{
				// we strip any trailing [0] above (which is useful for non-structure variables),
				// so we should not hit this path unless two variables exist like:
				// structure.member[0]
				// structure.member[1]
				// The program introspection should only return the first for a basic type,
				// and we should not hit this case
				parentmembers = NULL;
				RDCWARN("Unexpected naked array as member (expected only one [0], which should be trimmed");
				break;
			}
		}

		// construct a parent variable
		DynShaderConstant parentVar;
		parentVar.name = base;
		parentVar.reg.vec = var.reg.vec;
		parentVar.reg.comp = 0;
		parentVar.type.descriptor.name = "struct";
		parentVar.type.descriptor.rows = 0;
		parentVar.type.descriptor.cols = 0;
		parentVar.type.descriptor.rowMajorStorage = false;
		parentVar.type.descriptor.type = var.type.descriptor.type;
		parentVar.type.descriptor.elements = isarray ? RDCMAX(1U, uint32_t(arrayIdx+1)) : 0;

		bool found = false;

		// if we can find the base variable already, we recurse into its members
		for(size_t i=0; i < parentmembers->size(); i++)
		{
			if((*parentmembers)[i].name == base)
			{
				// if we find the variable, update the # elements to account for this new array index
				// and pick the minimum offset of all of our children as the parent offset. This is mostly
				// just for sorting
				(*parentmembers)[i].type.descriptor.elements =
					RDCMAX((*parentmembers)[i].type.descriptor.elements, parentVar.type.descriptor.elements);
				(*parentmembers)[i].reg.vec = RDCMIN((*parentmembers)[i].reg.vec, parentVar.reg.vec);

				parentmembers = &( (*parentmembers)[i].type.members );
				found = true;

				break;
			}
		}

		// if we didn't find the base variable, add it and recuse inside
		if(!found)
		{
			parentmembers->push_back(parentVar);
			parentmembers = &( parentmembers->back().type.members );
		}

		// the 0th element of each array fills out the actual members, when we
		// encounter an index above that we only use it to increase the type.descriptor.elements
		// member (which we've done by this point) and can stop recursing
		if(arrayIdx > 0)
		{
			parentmembers = NULL;
			break;
		}
	}

	if(parentmembers)
	{
		// nm points into var.name's storage, so copy out to a temporary
		string n = nm;
		var.name = n;

		parentmembers->push_back(var);
	}
}

void MakeShaderReflection(const GLHookSet &gl, GLenum shadType, GLuint sepProg, ShaderReflection &refl, bool pointSizeUsed, bool clipDistanceUsed)
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
		res.IsReadWrite = false;
		res.variableType.descriptor.rows = 1;
		res.variableType.descriptor.cols = 4;
		res.variableType.descriptor.elements = 0;
		res.variableType.descriptor.rowMajorStorage = false;
		res.bindPoint = (int32_t)resources.size();

		// float samplers
		if(values[0] == eGL_SAMPLER_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "samplerBuffer";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "sampler1D";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "sampler1DArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_1D_SHADOW)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "sampler1DShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_1D_ARRAY_SHADOW)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "sampler1DArrayShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2D";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "sampler2DArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_SHADOW)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "sampler2DShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_ARRAY_SHADOW)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "sampler2DArrayShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_RECT)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "sampler2DRect";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_RECT_SHADOW)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "sampler2DRectShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "sampler3D";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "samplerCube";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_CUBE_SHADOW)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "samplerCubeShadow";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "samplerCubeArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "sampler2DMS";
			res.variableType.descriptor.type = eVar_Float;
		}
		else if(values[0] == eGL_SAMPLER_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "sampler2DMSArray";
			res.variableType.descriptor.type = eVar_Float;
		}
		// int samplers
		else if(values[0] == eGL_INT_SAMPLER_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "isamplerBuffer";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "isampler1D";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "isampler1DArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "isampler2D";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "isampler2DArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_2D_RECT)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "isampler2DRect";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "isampler3D";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "isamplerCube";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "isamplerCubeArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "isampler2DMS";
			res.variableType.descriptor.type = eVar_Int;
		}
		else if(values[0] == eGL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "isampler2DMSArray";
			res.variableType.descriptor.type = eVar_Int;
		}
		// unsigned int samplers
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "usamplerBuffer";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "usampler1D";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "usampler1DArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "usampler2D";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "usampler2DArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_2D_RECT)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "usampler2DRect";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "usampler3D";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "usamplerCube";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "usamplerCubeArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "usampler2DMS";
			res.variableType.descriptor.type = eVar_UInt;
		}
		else if(values[0] == eGL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "usampler2DMSArray";
			res.variableType.descriptor.type = eVar_UInt;
		}
		// float images
		else if(values[0] == eGL_IMAGE_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "imageBuffer";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "image1D";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "image1DArray";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "image2D";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "image2DArray";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_2D_RECT)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "image2DRect";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "image3D";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "imageCube";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "imageCubeArray";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "image2DMS";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_IMAGE_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "image2DMSArray";
			res.variableType.descriptor.type = eVar_Float;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		// int images
		else if(values[0] == eGL_INT_IMAGE_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "iimageBuffer";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "iimage1D";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "iimage1DArray";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "iimage2D";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "iimage2DArray";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_2D_RECT)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "iimage2DRect";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "iimage3D";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "iimageCube";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "iimageCubeArray";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "iimage2DMS";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_INT_IMAGE_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "iimage2DMSArray";
			res.variableType.descriptor.type = eVar_Int;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		// unsigned int images
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_BUFFER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "uimageBuffer";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_1D)
		{
			res.resType = eResType_Texture1D;
			res.variableType.descriptor.name = "uimage1D";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_1D_ARRAY)
		{
			res.resType = eResType_Texture1DArray;
			res.variableType.descriptor.name = "uimage1DArray";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_2D)
		{
			res.resType = eResType_Texture2D;
			res.variableType.descriptor.name = "uimage2D";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_2D_ARRAY)
		{
			res.resType = eResType_Texture2DArray;
			res.variableType.descriptor.name = "uimage2DArray";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_2D_RECT)
		{
			res.resType = eResType_TextureRect;
			res.variableType.descriptor.name = "uimage2DRect";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_3D)
		{
			res.resType = eResType_Texture3D;
			res.variableType.descriptor.name = "uimage3D";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_CUBE)
		{
			res.resType = eResType_TextureCube;
			res.variableType.descriptor.name = "uimageCube";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY)
		{
			res.resType = eResType_TextureCubeArray;
			res.variableType.descriptor.name = "uimageCubeArray";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE)
		{
			res.resType = eResType_Texture2DMS;
			res.variableType.descriptor.name = "uimage2DMS";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		else if(values[0] == eGL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY)
		{
			res.resType = eResType_Texture2DMSArray;
			res.variableType.descriptor.name = "uimage2DMSArray";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
		}
		// atomic counter
		else if(values[0] == eGL_UNSIGNED_INT_ATOMIC_COUNTER)
		{
			res.resType = eResType_Buffer;
			res.variableType.descriptor.name = "atomic_uint";
			res.variableType.descriptor.type = eVar_UInt;
			res.IsReadWrite = true;
			res.IsSRV = false;
			res.IsTexture = false;
			res.variableType.descriptor.cols = 1;
		}
		else
		{
			// not a sampler
			continue;
		}

		char *namebuf = new char[values[1]+1];
		gl.glGetProgramResourceName(sepProg, eGL_UNIFORM, u, values[1], NULL, namebuf);
		namebuf[values[1]] = 0;

		string name = namebuf;

		res.name = name;

		resources.push_back(res);

		// array of samplers
		if(values[4] > 1)
		{
			name = name.substr(0, name.length()-3); // trim off [0] on the end
			for(int i=1; i < values[4]; i++)
			{
				string arrname = StringFormat::Fmt("%s[%d]", name.c_str(), i);
				
				res.bindPoint = (int32_t)resources.size();
				res.name = arrname;

				resources.push_back(res);
			}
		}
	}

	vector<int32_t> ssbos;
	uint32_t ssboMembers = 0;
	
	GLint numSSBOs = 0;
	{
		gl.glGetProgramInterfaceiv(sepProg, eGL_SHADER_STORAGE_BLOCK, eGL_ACTIVE_RESOURCES, &numSSBOs);

		for(GLint u=0; u < numSSBOs; u++)
		{
			GLenum propName = eGL_NAME_LENGTH;
			GLint len;
			gl.glGetProgramResourceiv(sepProg, eGL_SHADER_STORAGE_BLOCK, u, 1, &propName, 1, NULL, &len);

			char *nm = new char[len+1];
			gl.glGetProgramResourceName(sepProg, eGL_SHADER_STORAGE_BLOCK, u, len+1, NULL, nm);

			ShaderResource res;
			res.IsSampler = false;
			res.IsSRV = false;
			res.IsTexture = false;
			res.IsReadWrite = true;
			res.resType = eResType_Buffer;
			res.variableType.descriptor.rows = 0;
			res.variableType.descriptor.cols = 0;
			res.variableType.descriptor.elements = len;
			res.variableType.descriptor.rowMajorStorage = false;
			res.variableType.descriptor.name = "buffer";
			res.variableType.descriptor.type = eVar_UInt;
			res.bindPoint = (int32_t)resources.size();
			res.name = nm;
			
			propName = eGL_NUM_ACTIVE_VARIABLES;
			gl.glGetProgramResourceiv(sepProg, eGL_SHADER_STORAGE_BLOCK, u, 1, &propName, 1, NULL, (GLint *)&res.variableType.descriptor.elements);
			
			resources.push_back(res);
			ssbos.push_back(res.bindPoint);
			ssboMembers += res.variableType.descriptor.elements;

			delete[] nm;
		}
	}

	{
		vector<DynShaderConstant> *members = new vector<DynShaderConstant>[ssbos.size()];

		for(uint32_t i=0; i < ssboMembers; i++)
		{
			ReconstructVarTree(gl, eGL_BUFFER_VARIABLE, sepProg, i, (GLint)ssbos.size(), members, NULL);
		}

		for(size_t ssbo=0; ssbo < ssbos.size(); ssbo++)
		{
			sort(members[ssbo]);
			copy(resources[ ssbos[ssbo] ].variableType.members, members[ssbo]);
		}

		delete[] members;
	}

	vector<DynShaderConstant> globalUniforms;
	
	GLint numUBOs = 0;
	vector<string> uboNames;
	vector<DynShaderConstant> *ubos = NULL;
	
	{
		gl.glGetProgramInterfaceiv(sepProg, eGL_UNIFORM_BLOCK, eGL_ACTIVE_RESOURCES, &numUBOs);

		ubos = new vector<DynShaderConstant>[numUBOs];
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
		ReconstructVarTree(gl, eGL_UNIFORM, sepProg, u, numUBOs, ubos, &globalUniforms);
	}

	vector<ConstantBlock> cbuffers;
	
	if(ubos)
	{
		cbuffers.reserve(numUBOs + (globalUniforms.empty() ? 0 : 1));

		for(int i=0; i < numUBOs; i++)
		{
			if(!ubos[i].empty())
			{
				ConstantBlock cblock;
				cblock.name = uboNames[i];
				cblock.bufferBacked = true;
				cblock.bindPoint = (int32_t)cbuffers.size();

				sort(ubos[i]);
				copy(cblock.variables, ubos[i]);

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

		sort(globalUniforms);
		copy(globals.variables, globalUniforms);

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

				GLsizei numSigProps = (GLsizei)ARRAY_COUNT(props);

				// GL_LOCATION_COMPONENT not supported on core <4.4 (or without GL_ARB_enhanced_layouts)
				if(!ExtensionSupported[ExtensionSupported_ARB_enhanced_layouts] && GLCoreVersion < 44)
					numSigProps--;
				gl.glGetProgramResourceiv(sepProg, sigEnum, i, numSigProps, props, numSigProps, NULL, values);

				char *nm = new char[values[0]+1];
				gl.glGetProgramResourceName(sepProg, sigEnum, i, values[0]+1, NULL, nm);
				
				SigParameter sig;

				sig.varName = nm;
				sig.semanticIndex = 0;
				sig.needSemanticIndex = false;
				sig.stream = 0;

				int rows = 1;
				
				switch(values[1])
				{
					case eGL_FLOAT:
					case eGL_DOUBLE:
					case eGL_FLOAT_VEC2:
					case eGL_DOUBLE_VEC2:
					case eGL_FLOAT_VEC3:
					case eGL_DOUBLE_VEC3:
					case eGL_FLOAT_VEC4:
					case eGL_DOUBLE_VEC4:
					case eGL_FLOAT_MAT4:
					case eGL_DOUBLE_MAT4:
					case eGL_FLOAT_MAT4x3:
					case eGL_DOUBLE_MAT4x3:
					case eGL_FLOAT_MAT4x2:
					case eGL_DOUBLE_MAT4x2:
					case eGL_FLOAT_MAT3:
					case eGL_DOUBLE_MAT3:
					case eGL_FLOAT_MAT3x4:
					case eGL_DOUBLE_MAT3x4:
					case eGL_FLOAT_MAT3x2:
					case eGL_DOUBLE_MAT3x2:
					case eGL_FLOAT_MAT2:
					case eGL_DOUBLE_MAT2:
					case eGL_FLOAT_MAT2x3:
					case eGL_DOUBLE_MAT2x3:
					case eGL_FLOAT_MAT2x4:
					case eGL_DOUBLE_MAT2x4:
						sig.compType = eCompType_Float;
						break;
					case eGL_INT:
					case eGL_INT_VEC2:
					case eGL_INT_VEC3:
					case eGL_INT_VEC4:
						sig.compType = eCompType_SInt;
						break;
					case eGL_UNSIGNED_INT:
					case eGL_BOOL:
					case eGL_UNSIGNED_INT_VEC2:
					case eGL_BOOL_VEC2:
					case eGL_UNSIGNED_INT_VEC3:
					case eGL_BOOL_VEC3:
					case eGL_UNSIGNED_INT_VEC4:
					case eGL_BOOL_VEC4:
						sig.compType = eCompType_UInt;
						break;
					default:
						sig.compType = eCompType_Float;
						RDCWARN("Unhandled signature element type %s", ToStr::Get((GLenum)values[1]).c_str());
				}

				switch(values[1])
				{
					case eGL_FLOAT:
					case eGL_DOUBLE:
					case eGL_INT:
					case eGL_UNSIGNED_INT:
					case eGL_BOOL:
						sig.compCount = 1;
						sig.regChannelMask = 0x1;
						break;
					case eGL_FLOAT_VEC2:
					case eGL_DOUBLE_VEC2:
					case eGL_INT_VEC2:
					case eGL_UNSIGNED_INT_VEC2:
					case eGL_BOOL_VEC2:
						sig.compCount = 2;
						sig.regChannelMask = 0x3;
						break;
					case eGL_FLOAT_VEC3:
					case eGL_DOUBLE_VEC3:
					case eGL_INT_VEC3:
					case eGL_UNSIGNED_INT_VEC3:
					case eGL_BOOL_VEC3:
						sig.compCount = 3;
						sig.regChannelMask = 0x7;
						break;
					case eGL_FLOAT_VEC4:
					case eGL_DOUBLE_VEC4:
					case eGL_INT_VEC4:
					case eGL_UNSIGNED_INT_VEC4:
					case eGL_BOOL_VEC4:
						sig.compCount = 4;
						sig.regChannelMask = 0xf;
						break;
					case eGL_FLOAT_MAT4:
					case eGL_DOUBLE_MAT4:
						sig.compCount = 4;
						rows = 4;
						sig.regChannelMask = 0xf;
						break;
					case eGL_FLOAT_MAT4x3:
					case eGL_DOUBLE_MAT4x3:
						sig.compCount = 4;
						rows = 3;
						sig.regChannelMask = 0xf;
						break;
					case eGL_FLOAT_MAT4x2:
					case eGL_DOUBLE_MAT4x2:
						sig.compCount = 4;
						rows = 2;
						sig.regChannelMask = 0xf;
						break;
					case eGL_FLOAT_MAT3:
					case eGL_DOUBLE_MAT3:
						sig.compCount = 3;
						rows = 3;
						sig.regChannelMask = 0x7;
						break;
					case eGL_FLOAT_MAT3x4:
					case eGL_DOUBLE_MAT3x4:
						sig.compCount = 3;
						rows = 2;
						sig.regChannelMask = 0x7;
						break;
					case eGL_FLOAT_MAT3x2:
					case eGL_DOUBLE_MAT3x2:
						sig.compCount = 3;
						rows = 2;
						sig.regChannelMask = 0x7;
						break;
					case eGL_FLOAT_MAT2:
					case eGL_DOUBLE_MAT2:
						sig.compCount = 2;
						rows = 2;
						sig.regChannelMask = 0x3;
						break;
					case eGL_FLOAT_MAT2x3:
					case eGL_DOUBLE_MAT2x3:
						sig.compCount = 2;
						rows = 3;
						sig.regChannelMask = 0x3;
						break;
					case eGL_FLOAT_MAT2x4:
					case eGL_DOUBLE_MAT2x4:
						sig.compCount = 2;
						rows = 4;
						sig.regChannelMask = 0x3;
						break;
					default:
						RDCWARN("Unhandled signature element type %s", ToStr::Get((GLenum)values[1]).c_str());
						sig.compCount = 4;
						sig.regChannelMask = 0xf;
						break;
				}
			
				sig.regChannelMask <<= values[3];

				sig.channelUsedMask = sig.regChannelMask;

				sig.systemValue = eAttr_None;

#define IS_BUILTIN(builtin) !strncmp(nm, builtin, sizeof(builtin)-1)

				// if these weren't used, they were probably added just to make a separable program
				// (either by us or the program originally). Skip them from the output signature
				if(IS_BUILTIN("gl_PointSize") && !pointSizeUsed)
					continue;
				if(IS_BUILTIN("gl_ClipDistance") && !clipDistanceUsed)
					continue;

				// VS built-in inputs
				if(IS_BUILTIN("gl_VertexID"))             sig.systemValue = eAttr_VertexIndex;
				if(IS_BUILTIN("gl_InstanceID"))           sig.systemValue = eAttr_InstanceIndex;

				// VS built-in outputs
				if(IS_BUILTIN("gl_Position"))             sig.systemValue = eAttr_Position;
				if(IS_BUILTIN("gl_PointSize"))            sig.systemValue = eAttr_PointSize;
				if(IS_BUILTIN("gl_ClipDistance"))         sig.systemValue = eAttr_ClipDistance;
				
				// TCS built-in inputs
				if(IS_BUILTIN("gl_PatchVerticesIn"))      sig.systemValue = eAttr_PatchNumVertices;
				if(IS_BUILTIN("gl_PrimitiveID"))          sig.systemValue = eAttr_PrimitiveIndex;
				if(IS_BUILTIN("gl_InvocationID"))         sig.systemValue = eAttr_InvocationIndex;

				// TCS built-in outputs
				if(IS_BUILTIN("gl_TessLevelOuter"))       sig.systemValue = eAttr_OuterTessFactor;
				if(IS_BUILTIN("gl_TessLevelInner"))       sig.systemValue = eAttr_InsideTessFactor;
				
				// TES built-in inputs
				if(IS_BUILTIN("gl_TessCoord"))            sig.systemValue = eAttr_DomainLocation;
				if(IS_BUILTIN("gl_PatchVerticesIn"))      sig.systemValue = eAttr_PatchNumVertices;
				if(IS_BUILTIN("gl_PrimitiveID"))          sig.systemValue = eAttr_PrimitiveIndex;
				
				// GS built-in inputs
				if(IS_BUILTIN("gl_PrimitiveIDIn"))        sig.systemValue = eAttr_PrimitiveIndex;
				if(IS_BUILTIN("gl_InvocationID"))         sig.systemValue = eAttr_InvocationIndex;
				if(IS_BUILTIN("gl_Layer"))                sig.systemValue = eAttr_RTIndex;
				if(IS_BUILTIN("gl_ViewportIndex"))        sig.systemValue = eAttr_ViewportIndex;

				// GS built-in outputs
				if(IS_BUILTIN("gl_Layer"))                sig.systemValue = eAttr_RTIndex;
				if(IS_BUILTIN("gl_ViewportIndex"))        sig.systemValue = eAttr_ViewportIndex;
				
				// PS built-in inputs
				if(IS_BUILTIN("gl_FragCoord"))            sig.systemValue = eAttr_Position;
				if(IS_BUILTIN("gl_FrontFacing"))          sig.systemValue = eAttr_IsFrontFace;
				if(IS_BUILTIN("gl_PointCoord"))           sig.systemValue = eAttr_RTIndex;
				if(IS_BUILTIN("gl_SampleID"))             sig.systemValue = eAttr_MSAASampleIndex;
				if(IS_BUILTIN("gl_SamplePosition"))       sig.systemValue = eAttr_MSAASamplePosition;
				if(IS_BUILTIN("gl_SampleMaskIn"))         sig.systemValue = eAttr_MSAACoverage;
				
				// PS built-in outputs
				if(IS_BUILTIN("gl_FragDepth"))            sig.systemValue = eAttr_DepthOutput;
				if(IS_BUILTIN("gl_SampleMask"))           sig.systemValue = eAttr_MSAACoverage;
				
				// CS built-in inputs
				if(IS_BUILTIN("gl_NumWorkGroups"))        sig.systemValue = eAttr_DispatchSize;
				if(IS_BUILTIN("gl_WorkGroupID"))          sig.systemValue = eAttr_GroupIndex;
				if(IS_BUILTIN("gl_LocalInvocationID"))    sig.systemValue = eAttr_GroupThreadIndex;
				if(IS_BUILTIN("gl_GlobalInvocationID"))   sig.systemValue = eAttr_DispatchThreadIndex;
				if(IS_BUILTIN("gl_LocalInvocationIndex")) sig.systemValue = eAttr_GroupFlatIndex;

#undef IS_BUILTIN
				if(shadType == eGL_FRAGMENT_SHADER && sigEnum == eGL_PROGRAM_OUTPUT && sig.systemValue == eAttr_None)
					sig.systemValue = eAttr_ColourOutput;
				
				if(sig.systemValue == eAttr_None)
					sig.regIndex = values[2] >= 0 ? values[2] : i;
				else
					sig.regIndex = values[2] >= 0 ? values[2] : 0;

				if(rows == 1)
				{
					sigs.push_back(sig);
				}
				else
				{
					for(int r=0; r < rows; r++)
					{
						SigParameter s = sig;
						s.varName = StringFormat::Fmt("%s:row%d", nm, r);
						s.regIndex += r;
						sigs.push_back(s);
					}
				}

				delete[] nm;
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

	refl.Resources = resources;
	refl.ConstantBlocks = cbuffers;
}

void GetBindpointMapping(const GLHookSet &gl, GLuint curProg, int shadIdx, ShaderReflection *refl, ShaderBindpointMapping &mapping)
{
	// in case of bugs, we readback into this array instead of
	GLint dummyReadback[32];

#if !defined(RELEASE)
	for(size_t i=1; i < ARRAY_COUNT(dummyReadback); i++)
		dummyReadback[i] = 0x6c7b8a9d;
#endif

	const GLenum refEnum[] = {
		eGL_REFERENCED_BY_VERTEX_SHADER,
		eGL_REFERENCED_BY_TESS_CONTROL_SHADER,
		eGL_REFERENCED_BY_TESS_EVALUATION_SHADER,
		eGL_REFERENCED_BY_GEOMETRY_SHADER,
		eGL_REFERENCED_BY_FRAGMENT_SHADER,
		eGL_REFERENCED_BY_COMPUTE_SHADER,
	};
	
	create_array_uninit(mapping.Resources, refl->Resources.count);
	for(int32_t i=0; i < refl->Resources.count; i++)
	{
		if(refl->Resources.elems[i].IsTexture)
		{
			// normal sampler or image load/store

			GLint loc = gl.glGetUniformLocation(curProg, refl->Resources.elems[i].name.elems);
			if(loc >= 0)
			{
				gl.glGetUniformiv(curProg, loc, dummyReadback);
				mapping.Resources[i].bind = dummyReadback[0];
			}

			// handle sampler arrays, use the base name
			string name = refl->Resources.elems[i].name.elems;
			if(name.back() == ']')
			{
				do
				{
					name.pop_back();
				} while(name.back() != '[');
				name.pop_back();
			}
			
			GLuint idx = 0;
			idx = gl.glGetProgramResourceIndex(curProg, eGL_UNIFORM, name.c_str());

			if(idx == GL_INVALID_INDEX)
			{
				mapping.Resources[i].used = false;
			}
			else
			{
				GLint used = 0;
				gl.glGetProgramResourceiv(curProg, eGL_UNIFORM, idx, 1, &refEnum[shadIdx], 1, NULL, &used);
				mapping.Resources[i].used = (used != 0);
			}
		}
		else if(refl->Resources.elems[i].IsReadWrite && !refl->Resources.elems[i].IsTexture)
		{
			if(refl->Resources.elems[i].variableType.descriptor.cols == 1 &&
				refl->Resources.elems[i].variableType.descriptor.rows == 1 &&
				refl->Resources.elems[i].variableType.descriptor.type == eVar_UInt)
			{
				// atomic uint
				GLuint idx = gl.glGetProgramResourceIndex(curProg, eGL_UNIFORM, refl->Resources.elems[i].name.elems);

				if(idx == GL_INVALID_INDEX)
				{
					mapping.Resources[i].bind = -1;
					mapping.Resources[i].used = false;
				}
				else
				{
					GLenum prop = eGL_ATOMIC_COUNTER_BUFFER_INDEX;
					GLuint atomicIndex;
					gl.glGetProgramResourceiv(curProg, eGL_UNIFORM, idx, 1, &prop, 1, NULL, (GLint *)&atomicIndex);

					if(atomicIndex == GL_INVALID_INDEX)
					{
						mapping.Resources[i].bind = -1;
						mapping.Resources[i].used = false;
					}
					else
					{
						const GLenum atomicRefEnum[] = {
							eGL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_VERTEX_SHADER,
							eGL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_TESS_CONTROL_SHADER,
							eGL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_TESS_EVALUATION_SHADER,
							eGL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_GEOMETRY_SHADER,
							eGL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_FRAGMENT_SHADER,
							eGL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_COMPUTE_SHADER,
						};
						gl.glGetActiveAtomicCounterBufferiv(curProg, atomicIndex, eGL_ATOMIC_COUNTER_BUFFER_BINDING, &mapping.Resources[i].bind);
						GLint used = 0;
						gl.glGetActiveAtomicCounterBufferiv(curProg, atomicIndex, atomicRefEnum[shadIdx], &used);
						mapping.Resources[i].used = (used != 0);
					}
				}
			}
			else
			{
				// shader storage buffer object
				GLuint idx = gl.glGetProgramResourceIndex(curProg, eGL_SHADER_STORAGE_BLOCK, refl->Resources.elems[i].name.elems);

				if(idx == GL_INVALID_INDEX)
				{
					mapping.Resources[i].bind = -1;
					mapping.Resources[i].used = false;
				}
				else
				{
					GLenum prop = eGL_BUFFER_BINDING;
					gl.glGetProgramResourceiv(curProg, eGL_SHADER_STORAGE_BLOCK, idx, 1, &prop, 1, NULL, &mapping.Resources[i].bind);
					GLint used = 0;
					gl.glGetProgramResourceiv(curProg, eGL_SHADER_STORAGE_BLOCK, idx, 1, &refEnum[shadIdx], 1, NULL, &used);
					mapping.Resources[i].used = (used != 0);
				}
			}
		}
		else
		{
			mapping.Resources[i].bind = -1;
			mapping.Resources[i].used = false;
		}
	}
	
	create_array_uninit(mapping.ConstantBlocks, refl->ConstantBlocks.count);
	for(int32_t i=0; i < refl->ConstantBlocks.count; i++)
	{
		if(refl->ConstantBlocks.elems[i].bufferBacked)
		{
			GLint loc = gl.glGetUniformBlockIndex(curProg, refl->ConstantBlocks.elems[i].name.elems);
			if(loc >= 0)
			{
				gl.glGetActiveUniformBlockiv(curProg, loc, eGL_UNIFORM_BLOCK_BINDING, dummyReadback);
				mapping.ConstantBlocks[i].bind = dummyReadback[0];
			}
		}
		else
		{
			mapping.ConstantBlocks[i].bind = -1;
		}

		if(!refl->ConstantBlocks.elems[i].bufferBacked)
		{
			mapping.ConstantBlocks[i].used = true;
		}
		else
		{
			GLuint idx = gl.glGetProgramResourceIndex(curProg, eGL_UNIFORM_BLOCK, refl->ConstantBlocks.elems[i].name.elems);
			if(idx == GL_INVALID_INDEX)
			{
				mapping.ConstantBlocks[i].used = false;
			}
			else
			{
				GLint used = 0;
				gl.glGetProgramResourceiv(curProg, eGL_UNIFORM_BLOCK, idx, 1, &refEnum[shadIdx], 1, NULL, &used);
				mapping.ConstantBlocks[i].used = (used != 0);
			}
		}
	}
	
	GLint numVAttribBindings = 16;
	gl.glGetIntegerv(eGL_MAX_VERTEX_ATTRIBS, &numVAttribBindings);

	create_array_uninit(mapping.InputAttributes, numVAttribBindings);
	for(int32_t i=0; i < numVAttribBindings; i++)
		mapping.InputAttributes[i] = -1;

	// override identity map with bindings
	if(shadIdx == 0)
	{
		for(int32_t i=0; i < refl->InputSig.count; i++)
		{
			GLint loc = gl.glGetAttribLocation(curProg, refl->InputSig.elems[i].varName.elems);

			if(loc >= 0 && loc < numVAttribBindings)
			{
				mapping.InputAttributes[loc] = i;
			}
		}
	}

#if !defined(RELEASE)
	for(size_t i=1; i < ARRAY_COUNT(dummyReadback); i++)
		if(dummyReadback[i] != 0x6c7b8a9d)
			RDCERR("Invalid uniform readback - data beyond first element modified!");
#endif
}

TBuiltInResource DefaultResources = {
  /*.maxLights =*/ 32,
  /*.maxClipPlanes =*/ 6,
  /*.maxTextureUnits =*/ 32,
  /*.maxTextureCoords =*/ 32,
  /*.maxVertexAttribs =*/ 64,
  /*.maxVertexUniformComponents =*/ 4096,
  /*.maxVaryingFloats =*/ 64,
  /*.maxVertexTextureImageUnits =*/ 32,
  /*.maxCombinedTextureImageUnits =*/ 80,
  /*.maxTextureImageUnits =*/ 32,
  /*.maxFragmentUniformComponents =*/ 4096,
  /*.maxDrawBuffers =*/ 32,
  /*.maxVertexUniformVectors =*/ 128,
  /*.maxVaryingVectors =*/ 8,
  /*.maxFragmentUniformVectors =*/ 16,
  /*.maxVertexOutputVectors =*/ 16,
  /*.maxFragmentInputVectors =*/ 15,
  /*.minProgramTexelOffset =*/ -8,
  /*.maxProgramTexelOffset =*/ 7,
  /*.maxClipDistances =*/ 8,
  /*.maxComputeWorkGroupCountX =*/ 65535,
  /*.maxComputeWorkGroupCountY =*/ 65535,
  /*.maxComputeWorkGroupCountZ =*/ 65535,
  /*.maxComputeWorkGroupSizeX =*/ 1024,
  /*.maxComputeWorkGroupSizeY =*/ 1024,
  /*.maxComputeWorkGroupSizeZ =*/ 64,
  /*.maxComputeUniformComponents =*/ 1024,
  /*.maxComputeTextureImageUnits =*/ 16,
  /*.maxComputeImageUniforms =*/ 8,
  /*.maxComputeAtomicCounters =*/ 8,
  /*.maxComputeAtomicCounterBuffers =*/ 1,
  /*.maxVaryingComponents =*/ 60,
  /*.maxVertexOutputComponents =*/ 64,
  /*.maxGeometryInputComponents =*/ 64,
  /*.maxGeometryOutputComponents =*/ 128,
  /*.maxFragmentInputComponents =*/ 128,
  /*.maxImageUnits =*/ 8,
  /*.maxCombinedImageUnitsAndFragmentOutputs =*/ 8,
  /*.maxCombinedShaderOutputResources =*/ 8,
  /*.maxImageSamples =*/ 0,
  /*.maxVertexImageUniforms =*/ 0,
  /*.maxTessControlImageUniforms =*/ 0,
  /*.maxTessEvaluationImageUniforms =*/ 0,
  /*.maxGeometryImageUniforms =*/ 0,
  /*.maxFragmentImageUniforms =*/ 8,
  /*.maxCombinedImageUniforms =*/ 8,
  /*.maxGeometryTextureImageUnits =*/ 16,
  /*.maxGeometryOutputVertices =*/ 256,
  /*.maxGeometryTotalOutputComponents =*/ 1024,
  /*.maxGeometryUniformComponents =*/ 1024,
  /*.maxGeometryVaryingComponents =*/ 64,
  /*.maxTessControlInputComponents =*/ 128,
  /*.maxTessControlOutputComponents =*/ 128,
  /*.maxTessControlTextureImageUnits =*/ 16,
  /*.maxTessControlUniformComponents =*/ 1024,
  /*.maxTessControlTotalOutputComponents =*/ 4096,
  /*.maxTessEvaluationInputComponents =*/ 128,
  /*.maxTessEvaluationOutputComponents =*/ 128,
  /*.maxTessEvaluationTextureImageUnits =*/ 16,
  /*.maxTessEvaluationUniformComponents =*/ 1024,
  /*.maxTessPatchComponents =*/ 120,
  /*.maxPatchVertices =*/ 32,
  /*.maxTessGenLevel =*/ 64,
  /*.maxViewports =*/ 16,
  /*.maxVertexAtomicCounters =*/ 0,
  /*.maxTessControlAtomicCounters =*/ 0,
  /*.maxTessEvaluationAtomicCounters =*/ 0,
  /*.maxGeometryAtomicCounters =*/ 0,
  /*.maxFragmentAtomicCounters =*/ 8,
  /*.maxCombinedAtomicCounters =*/ 8,
  /*.maxAtomicCounterBindings =*/ 1,
  /*.maxVertexAtomicCounterBuffers =*/ 0,
  /*.maxTessControlAtomicCounterBuffers =*/ 0,
  /*.maxTessEvaluationAtomicCounterBuffers =*/ 0,
  /*.maxGeometryAtomicCounterBuffers =*/ 0,
  /*.maxFragmentAtomicCounterBuffers =*/ 1,
  /*.maxCombinedAtomicCounterBuffers =*/ 1,
  /*.maxAtomicCounterBufferSize =*/ 16384,
  /*.maxTransformFeedbackBuffers =*/ 4,
  /*.maxTransformFeedbackInterleavedComponents =*/ 64,
  /*.maxCullDistances =*/ 8,
  /*.maxCombinedClipAndCullDistances =*/ 8,
  /*.maxSamples =*/ 4,

  /*.limits.nonInductiveForLoops =*/ 1,
  /*.limits.whileLoops =*/ 1,
  /*.limits.doWhileLoops =*/ 1,
  /*.limits.generalUniformIndexing =*/ 1,
  /*.limits.generalAttributeMatrixVectorIndexing =*/ 1,
  /*.limits.generalVaryingIndexing =*/ 1,
  /*.limits.generalSamplerIndexing =*/ 1,
  /*.limits.generalVariableIndexing =*/ 1,
  /*.limits.generalConstantMatrixVectorIndexing =*/ 1
};

string CompileSPIRV(GLenum shadType, const std::vector<std::string> &sources, vector<uint32_t> &spirv)
{
	string errors = "";

	const char **strs = new const char *[sources.size()];

	for(size_t i=0; i < sources.size(); i++)
		strs[i] = sources[i].c_str();

	{
		EShLanguage lang = EShLanguage(EShLangVertex + (int)ShaderIdx(shadType));

		glslang::TShader *shader = new glslang::TShader(lang);

		shader->setStrings(strs, (int)sources.size());

		bool success = shader->parse(&DefaultResources, 110, false, EShMsgDefault);

		if(!success)
		{
			errors = "Shader failed to compile:\n\n";
			errors += shader->getInfoLog();
			errors += "\n\n";
			errors += shader->getInfoDebugLog();
		}
		else
		{
			glslang::TProgram *program = new glslang::TProgram();

			program->addShader(shader);

			success = program->link(EShMsgDefault);

			if(!success)
			{
				errors = "Program failed to link:\n\n";
				errors += program->getInfoLog();
				errors += "\n\n";
				errors += program->getInfoDebugLog();
			}
			else
			{
				glslang::TIntermediate *intermediate = program->getIntermediate(lang);

				// if we successfully compiled and linked, we must have the stage we started with
				RDCASSERT(intermediate);

				glslang::GlslangToSpv(*intermediate, spirv);
			}

			delete program;
		}

		delete shader;
	}

	delete[] strs;

	return errors;
}

void DisassembleSPIRV(GLenum shadType, const vector<uint32_t> &spirv, string &disasm)
{
	// temporary function until we build our own structure from the SPIR-V
	const char *header[] = {
		"Vertex Shader",
		"Tessellation Control Shader",
		"Tessellation Evaluation Shader",
		"Geometry Shader",
		"Fragment Shader",
		"Compute Shader",
	};

	disasm = header[ShaderIdx(shadType)];
	disasm += " SPIR-V:\n\n";

	if(spirv[0] != spv::MagicNumber)
	{
		disasm += StringFormat::Fmt("Unrecognised magic number %08x", spirv[0]);
		return;
	}

	const char *gen = "Unrecognised";

	// list of known generators, just for kicks
	struct { uint32_t magic; const char *name; } gens[] = {
		0x051a00bb, "glslang",
	};

	for(size_t i=0; i < ARRAY_COUNT(gens); i++) if(gens[i].magic == spirv[2])	gen = gens[i].name;
	
	disasm += StringFormat::Fmt("Version %u, Generator %08x (%s)\n", spirv[1], spirv[2], gen);
	disasm += StringFormat::Fmt("IDs up to <%u>\n", spirv[3]);

	uint32_t idbound = spirv[3];

	if(spirv[4] != 0) disasm += "Reserved word 4 is non-zero\n";

	disasm += "\n";

	uint32_t opidx = 0;
	bool infunc = false;

	vector<string> resultnames;
	resultnames.resize(idbound);

	// fetch names and things to be used in the second pass
	size_t it = 5;
	while(it < spirv.size())
	{
		uint16_t WordCount = spirv[it]>>16;
		spv::Op OpCode = spv::Op(spirv[it]&0xffff);

		if(OpCode == spv::OpName)
			resultnames[ spirv[it+1] ] = (const char *)&spirv[it+2];
		
		it += WordCount;
	}

	for(size_t i=0; i < resultnames.size(); i++)
		if(resultnames[i].empty())
			resultnames[i] = StringFormat::Fmt("<%d>", i);

	it = 5;
	while(it < spirv.size())
	{
		uint16_t WordCount = spirv[it]>>16;
		spv::Op OpCode = spv::Op(spirv[it]&0xffff);

		string body;
		bool silent = false;

		switch(OpCode)
		{
			case spv::OpSource:
				body = StringFormat::Fmt("%s %d", ToStr::Get(spv::SourceLanguage(spirv[it+1])).c_str(), spirv[it+2]);
				break;
			case spv::OpExtInstImport:
				resultnames[ spirv[it+1] ] = (char *)&spirv[it+2];
				body = StringFormat::Fmt("%s", (char *)&spirv[it+2]);
				break;
			case spv::OpMemoryModel:
				body = StringFormat::Fmt("%s Addressing, %s Memory model",
					ToStr::Get(spv::AddressingModel(spirv[it+1])).c_str(),
					ToStr::Get(spv::MemoryModel(spirv[it+2])).c_str());
				break;
			case spv::OpEntryPoint:
				body = StringFormat::Fmt("%s (%s)",
					resultnames[ spirv[it+2] ].c_str(),
					ToStr::Get(spv::ExecutionModel(spirv[it+1])).c_str());
				break;
			case spv::OpFunction:
				infunc = true;
				break;
			case spv::OpFunctionEnd:
				infunc = false;
				break;
			case spv::OpName:
				silent = true;
				break;
			default:
				break;
		}
		
		if(infunc)
		{
			disasm += StringFormat::Fmt("% 4u: %s %s\n", opidx, ToStr::Get(OpCode).c_str(), body.c_str());
			opidx++;
		}
		else if(!silent)
		{
			disasm += StringFormat::Fmt("      %s %s\n",        ToStr::Get(OpCode).c_str(), body.c_str());
		}

		it += WordCount;
	}
}

string ToStrHelper<false, spv::Op>::Get(const spv::Op &el)
{
	switch(el)
	{
		case spv::OpNop:                                      return "Nop";
		case spv::OpSource:                                   return "Source";
		case spv::OpSourceExtension:                          return "SourceExtension";
		case spv::OpExtension:                                return "Extension";
		case spv::OpExtInstImport:                            return "ExtInstImport";
		case spv::OpMemoryModel:                              return "MemoryModel";
		case spv::OpEntryPoint:                               return "EntryPoint";
		case spv::OpExecutionMode:                            return "ExecutionMode";
		case spv::OpTypeVoid:                                 return "TypeVoid";
		case spv::OpTypeBool:                                 return "TypeBool";
		case spv::OpTypeInt:                                  return "TypeInt";
		case spv::OpTypeFloat:                                return "TypeFloat";
		case spv::OpTypeVector:                               return "TypeVector";
		case spv::OpTypeMatrix:                               return "TypeMatrix";
		case spv::OpTypeSampler:                              return "TypeSampler";
		case spv::OpTypeFilter:                               return "TypeFilter";
		case spv::OpTypeArray:                                return "TypeArray";
		case spv::OpTypeRuntimeArray:                         return "TypeRuntimeArray";
		case spv::OpTypeStruct:                               return "TypeStruct";
		case spv::OpTypeOpaque:                               return "TypeOpaque";
		case spv::OpTypePointer:                              return "TypePointer";
		case spv::OpTypeFunction:                             return "TypeFunction";
		case spv::OpTypeEvent:                                return "TypeEvent";
		case spv::OpTypeDeviceEvent:                          return "TypeDeviceEvent";
		case spv::OpTypeReserveId:                            return "TypeReserveId";
		case spv::OpTypeQueue:                                return "TypeQueue";
		case spv::OpTypePipe:                                 return "TypePipe";
		case spv::OpConstantTrue:                             return "ConstantTrue";
		case spv::OpConstantFalse:                            return "ConstantFalse";
		case spv::OpConstant:                                 return "Constant";
		case spv::OpConstantComposite:                        return "ConstantComposite";
		case spv::OpConstantSampler:                          return "ConstantSampler";
		case spv::OpConstantNullPointer:                      return "ConstantNullPointer";
		case spv::OpConstantNullObject:                       return "ConstantNullObject";
		case spv::OpSpecConstantTrue:                         return "SpecConstantTrue";
		case spv::OpSpecConstantFalse:                        return "SpecConstantFalse";
		case spv::OpSpecConstant:                             return "SpecConstant";
		case spv::OpSpecConstantComposite:                    return "SpecConstantComposite";
		case spv::OpVariable:                                 return "Variable";
		case spv::OpVariableArray:                            return "VariableArray";
		case spv::OpFunction:                                 return "Function";
		case spv::OpFunctionParameter:                        return "FunctionParameter";
		case spv::OpFunctionEnd:                              return "FunctionEnd";
		case spv::OpFunctionCall:                             return "FunctionCall";
		case spv::OpExtInst:                                  return "ExtInst";
		case spv::OpUndef:                                    return "Undef";
		case spv::OpLoad:                                     return "Load";
		case spv::OpStore:                                    return "Store";
		case spv::OpPhi:                                      return "Phi";
		case spv::OpDecorationGroup:                          return "DecorationGroup";
		case spv::OpDecorate:                                 return "Decorate";
		case spv::OpMemberDecorate:                           return "MemberDecorate";
		case spv::OpGroupDecorate:                            return "GroupDecorate";
		case spv::OpGroupMemberDecorate:                      return "GroupMemberDecorate";
		case spv::OpName:                                     return "Name";
		case spv::OpMemberName:                               return "MemberName";
		case spv::OpString:                                   return "String";
		case spv::OpLine:                                     return "Line";
		case spv::OpVectorExtractDynamic:                     return "VectorExtractDynamic";
		case spv::OpVectorInsertDynamic:                      return "VectorInsertDynamic";
		case spv::OpVectorShuffle:                            return "VectorShuffle";
		case spv::OpCompositeConstruct:                       return "CompositeConstruct";
		case spv::OpCompositeExtract:                         return "CompositeExtract";
		case spv::OpCompositeInsert:                          return "CompositeInsert";
		case spv::OpCopyObject:                               return "CopyObject";
		case spv::OpCopyMemory:                               return "CopyMemory";
		case spv::OpCopyMemorySized:                          return "CopyMemorySized";
		case spv::OpSampler:                                  return "Sampler";
		case spv::OpTextureSample:                            return "TextureSample";
		case spv::OpTextureSampleDref:                        return "TextureSampleDref";
		case spv::OpTextureSampleLod:                         return "TextureSampleLod";
		case spv::OpTextureSampleProj:                        return "TextureSampleProj";
		case spv::OpTextureSampleGrad:                        return "TextureSampleGrad";
		case spv::OpTextureSampleOffset:                      return "TextureSampleOffset";
		case spv::OpTextureSampleProjLod:                     return "TextureSampleProjLod";
		case spv::OpTextureSampleProjGrad:                    return "TextureSampleProjGrad";
		case spv::OpTextureSampleLodOffset:                   return "TextureSampleLodOffset";
		case spv::OpTextureSampleProjOffset:                  return "TextureSampleProjOffset";
		case spv::OpTextureSampleGradOffset:                  return "TextureSampleGradOffset";
		case spv::OpTextureSampleProjLodOffset:               return "TextureSampleProjLodOffset";
		case spv::OpTextureSampleProjGradOffset:              return "TextureSampleProjGradOffset";
		case spv::OpTextureFetchTexelLod:                     return "TextureFetchTexelLod";
		case spv::OpTextureFetchTexelOffset:                  return "TextureFetchTexelOffset";
		case spv::OpTextureFetchSample:                       return "TextureFetchSample";
		case spv::OpTextureFetchTexel:                        return "TextureFetchTexel";
		case spv::OpTextureGather:                            return "TextureGather";
		case spv::OpTextureGatherOffset:                      return "TextureGatherOffset";
		case spv::OpTextureGatherOffsets:                     return "TextureGatherOffsets";
		case spv::OpTextureQuerySizeLod:                      return "TextureQuerySizeLod";
		case spv::OpTextureQuerySize:                         return "TextureQuerySize";
		case spv::OpTextureQueryLod:                          return "TextureQueryLod";
		case spv::OpTextureQueryLevels:                       return "TextureQueryLevels";
		case spv::OpTextureQuerySamples:                      return "TextureQuerySamples";
		case spv::OpAccessChain:                              return "AccessChain";
		case spv::OpInBoundsAccessChain:                      return "InBoundsAccessChain";
		case spv::OpSNegate:                                  return "SNegate";
		case spv::OpFNegate:                                  return "FNegate";
		case spv::OpNot:                                      return "Not";
		case spv::OpAny:                                      return "Any";
		case spv::OpAll:                                      return "All";
		case spv::OpConvertFToU:                              return "ConvertFToU";
		case spv::OpConvertFToS:                              return "ConvertFToS";
		case spv::OpConvertSToF:                              return "ConvertSToF";
		case spv::OpConvertUToF:                              return "ConvertUToF";
		case spv::OpUConvert:                                 return "UConvert";
		case spv::OpSConvert:                                 return "SConvert";
		case spv::OpFConvert:                                 return "FConvert";
		case spv::OpConvertPtrToU:                            return "ConvertPtrToU";
		case spv::OpConvertUToPtr:                            return "ConvertUToPtr";
		case spv::OpPtrCastToGeneric:                         return "PtrCastToGeneric";
		case spv::OpGenericCastToPtr:                         return "GenericCastToPtr";
		case spv::OpBitcast:                                  return "Bitcast";
		case spv::OpTranspose:                                return "Transpose";
		case spv::OpIsNan:                                    return "IsNan";
		case spv::OpIsInf:                                    return "IsInf";
		case spv::OpIsFinite:                                 return "IsFinite";
		case spv::OpIsNormal:                                 return "IsNormal";
		case spv::OpSignBitSet:                               return "SignBitSet";
		case spv::OpLessOrGreater:                            return "LessOrGreater";
		case spv::OpOrdered:                                  return "Ordered";
		case spv::OpUnordered:                                return "Unordered";
		case spv::OpArrayLength:                              return "ArrayLength";
		case spv::OpIAdd:                                     return "IAdd";
		case spv::OpFAdd:                                     return "FAdd";
		case spv::OpISub:                                     return "ISub";
		case spv::OpFSub:                                     return "FSub";
		case spv::OpIMul:                                     return "IMul";
		case spv::OpFMul:                                     return "FMul";
		case spv::OpUDiv:                                     return "UDiv";
		case spv::OpSDiv:                                     return "SDiv";
		case spv::OpFDiv:                                     return "FDiv";
		case spv::OpUMod:                                     return "UMod";
		case spv::OpSRem:                                     return "SRem";
		case spv::OpSMod:                                     return "SMod";
		case spv::OpFRem:                                     return "FRem";
		case spv::OpFMod:                                     return "FMod";
		case spv::OpVectorTimesScalar:                        return "VectorTimesScalar";
		case spv::OpMatrixTimesScalar:                        return "MatrixTimesScalar";
		case spv::OpVectorTimesMatrix:                        return "VectorTimesMatrix";
		case spv::OpMatrixTimesVector:                        return "MatrixTimesVector";
		case spv::OpMatrixTimesMatrix:                        return "MatrixTimesMatrix";
		case spv::OpOuterProduct:                             return "OuterProduct";
		case spv::OpDot:                                      return "Dot";
		case spv::OpShiftRightLogical:                        return "ShiftRightLogical";
		case spv::OpShiftRightArithmetic:                     return "ShiftRightArithmetic";
		case spv::OpShiftLeftLogical:                         return "ShiftLeftLogical";
		case spv::OpLogicalOr:                                return "LogicalOr";
		case spv::OpLogicalXor:                               return "LogicalXor";
		case spv::OpLogicalAnd:                               return "LogicalAnd";
		case spv::OpBitwiseOr:                                return "BitwiseOr";
		case spv::OpBitwiseXor:                               return "BitwiseXor";
		case spv::OpBitwiseAnd:                               return "BitwiseAnd";
		case spv::OpSelect:                                   return "Select";
		case spv::OpIEqual:                                   return "IEqual";
		case spv::OpFOrdEqual:                                return "FOrdEqual";
		case spv::OpFUnordEqual:                              return "FUnordEqual";
		case spv::OpINotEqual:                                return "INotEqual";
		case spv::OpFOrdNotEqual:                             return "FOrdNotEqual";
		case spv::OpFUnordNotEqual:                           return "FUnordNotEqual";
		case spv::OpULessThan:                                return "ULessThan";
		case spv::OpSLessThan:                                return "SLessThan";
		case spv::OpFOrdLessThan:                             return "FOrdLessThan";
		case spv::OpFUnordLessThan:                           return "FUnordLessThan";
		case spv::OpUGreaterThan:                             return "UGreaterThan";
		case spv::OpSGreaterThan:                             return "SGreaterThan";
		case spv::OpFOrdGreaterThan:                          return "FOrdGreaterThan";
		case spv::OpFUnordGreaterThan:                        return "FUnordGreaterThan";
		case spv::OpULessThanEqual:                           return "ULessThanEqual";
		case spv::OpSLessThanEqual:                           return "SLessThanEqual";
		case spv::OpFOrdLessThanEqual:                        return "FOrdLessThanEqual";
		case spv::OpFUnordLessThanEqual:                      return "FUnordLessThanEqual";
		case spv::OpUGreaterThanEqual:                        return "UGreaterThanEqual";
		case spv::OpSGreaterThanEqual:                        return "SGreaterThanEqual";
		case spv::OpFOrdGreaterThanEqual:                     return "FOrdGreaterThanEqual";
		case spv::OpFUnordGreaterThanEqual:                   return "FUnordGreaterThanEqual";
		case spv::OpDPdx:                                     return "DPdx";
		case spv::OpDPdy:                                     return "DPdy";
		case spv::OpFwidth:                                   return "Fwidth";
		case spv::OpDPdxFine:                                 return "DPdxFine";
		case spv::OpDPdyFine:                                 return "DPdyFine";
		case spv::OpFwidthFine:                               return "FwidthFine";
		case spv::OpDPdxCoarse:                               return "DPdxCoarse";
		case spv::OpDPdyCoarse:                               return "DPdyCoarse";
		case spv::OpFwidthCoarse:                             return "FwidthCoarse";
		case spv::OpEmitVertex:                               return "EmitVertex";
		case spv::OpEndPrimitive:                             return "EndPrimitive";
		case spv::OpEmitStreamVertex:                         return "EmitStreamVertex";
		case spv::OpEndStreamPrimitive:                       return "EndStreamPrimitive";
		case spv::OpControlBarrier:                           return "ControlBarrier";
		case spv::OpMemoryBarrier:                            return "MemoryBarrier";
		case spv::OpImagePointer:                             return "ImagePointer";
		case spv::OpAtomicInit:                               return "AtomicInit";
		case spv::OpAtomicLoad:                               return "AtomicLoad";
		case spv::OpAtomicStore:                              return "AtomicStore";
		case spv::OpAtomicExchange:                           return "AtomicExchange";
		case spv::OpAtomicCompareExchange:                    return "AtomicCompareExchange";
		case spv::OpAtomicCompareExchangeWeak:                return "AtomicCompareExchangeWeak";
		case spv::OpAtomicIIncrement:                         return "AtomicIIncrement";
		case spv::OpAtomicIDecrement:                         return "AtomicIDecrement";
		case spv::OpAtomicIAdd:                               return "AtomicIAdd";
		case spv::OpAtomicISub:                               return "AtomicISub";
		case spv::OpAtomicUMin:                               return "AtomicUMin";
		case spv::OpAtomicUMax:                               return "AtomicUMax";
		case spv::OpAtomicAnd:                                return "AtomicAnd";
		case spv::OpAtomicOr:                                 return "AtomicOr";
		case spv::OpAtomicXor:                                return "AtomicXor";
		case spv::OpLoopMerge:                                return "LoopMerge";
		case spv::OpSelectionMerge:                           return "SelectionMerge";
		case spv::OpLabel:                                    return "Label";
		case spv::OpBranch:                                   return "Branch";
		case spv::OpBranchConditional:                        return "BranchConditional";
		case spv::OpSwitch:                                   return "Switch";
		case spv::OpKill:                                     return "Kill";
		case spv::OpReturn:                                   return "Return";
		case spv::OpReturnValue:                              return "ReturnValue";
		case spv::OpUnreachable:                              return "Unreachable";
		case spv::OpLifetimeStart:                            return "LifetimeStart";
		case spv::OpLifetimeStop:                             return "LifetimeStop";
		case spv::OpCompileFlag:                              return "CompileFlag";
		case spv::OpAsyncGroupCopy:                           return "AsyncGroupCopy";
		case spv::OpWaitGroupEvents:                          return "WaitGroupEvents";
		case spv::OpGroupAll:                                 return "GroupAll";
		case spv::OpGroupAny:                                 return "GroupAny";
		case spv::OpGroupBroadcast:                           return "GroupBroadcast";
		case spv::OpGroupIAdd:                                return "GroupIAdd";
		case spv::OpGroupFAdd:                                return "GroupFAdd";
		case spv::OpGroupFMin:                                return "GroupFMin";
		case spv::OpGroupUMin:                                return "GroupUMin";
		case spv::OpGroupSMin:                                return "GroupSMin";
		case spv::OpGroupFMax:                                return "GroupFMax";
		case spv::OpGroupUMax:                                return "GroupUMax";
		case spv::OpGroupSMax:                                return "GroupSMax";
		case spv::OpGenericCastToPtrExplicit:                 return "GenericCastToPtrExplicit";
		case spv::OpGenericPtrMemSemantics:                   return "GenericPtrMemSemantics";
		case spv::OpReadPipe:                                 return "ReadPipe";
		case spv::OpWritePipe:                                return "WritePipe";
		case spv::OpReservedReadPipe:                         return "ReservedReadPipe";
		case spv::OpReservedWritePipe:                        return "ReservedWritePipe";
		case spv::OpReserveReadPipePackets:                   return "ReserveReadPipePackets";
		case spv::OpReserveWritePipePackets:                  return "ReserveWritePipePackets";
		case spv::OpCommitReadPipe:                           return "CommitReadPipe";
		case spv::OpCommitWritePipe:                          return "CommitWritePipe";
		case spv::OpIsValidReserveId:                         return "IsValidReserveId";
		case spv::OpGetNumPipePackets:                        return "GetNumPipePackets";
		case spv::OpGetMaxPipePackets:                        return "GetMaxPipePackets";
		case spv::OpGroupReserveReadPipePackets:              return "GroupReserveReadPipePackets";
		case spv::OpGroupReserveWritePipePackets:             return "GroupReserveWritePipePackets";
		case spv::OpGroupCommitReadPipe:                      return "GroupCommitReadPipe";
		case spv::OpGroupCommitWritePipe:                     return "GroupCommitWritePipe";
		case spv::OpEnqueueMarker:                            return "EnqueueMarker";
		case spv::OpEnqueueKernel:                            return "EnqueueKernel";
		case spv::OpGetKernelNDrangeSubGroupCount:            return "GetKernelNDrangeSubGroupCount";
		case spv::OpGetKernelNDrangeMaxSubGroupSize:          return "GetKernelNDrangeMaxSubGroupSize";
		case spv::OpGetKernelWorkGroupSize:                   return "GetKernelWorkGroupSize";
		case spv::OpGetKernelPreferredWorkGroupSizeMultiple:  return "GetKernelPreferredWorkGroupSizeMultiple";
		case spv::OpRetainEvent:                              return "RetainEvent";
		case spv::OpReleaseEvent:                             return "ReleaseEvent";
		case spv::OpCreateUserEvent:                          return "CreateUserEvent";
		case spv::OpIsValidEvent:                             return "IsValidEvent";
		case spv::OpSetUserEventStatus:                       return "SetUserEventStatus";
		case spv::OpCaptureEventProfilingInfo:                return "CaptureEventProfilingInfo";
		case spv::OpGetDefaultQueue:                          return "GetDefaultQueue";
		case spv::OpBuildNDRange:                             return "BuildNDRange";
		case spv::OpSatConvertSToU:                           return "SatConvertSToU";
		case spv::OpSatConvertUToS:                           return "SatConvertUToS";
		case spv::OpAtomicIMin:                               return "AtomicIMin";
		case spv::OpAtomicIMax:                               return "AtomicIMax";
		default: break;
	}
	
	return "Unrecognised";
}

string ToStrHelper<false, spv::SourceLanguage>::Get(const spv::SourceLanguage &el)
{
	switch(el)
	{
		case spv::SourceLanguageUnknown: return "Unknown";
		case spv::SourceLanguageESSL:    return "ESSL";
		case spv::SourceLanguageGLSL:    return "GLSL";
		case spv::SourceLanguageOpenCL:  return "OpenCL";
		default: break;
	}
	
	return "Unrecognised";
}

string ToStrHelper<false, spv::AddressingModel>::Get(const spv::AddressingModel &el)
{
	switch(el)
	{
		case spv::AddressingModelLogical:    return "Logical";
		case spv::AddressingModelPhysical32: return "Physical (32-bit)";
		case spv::AddressingModelPhysical64: return "Physical (64-bit)";
		default: break;
	}
	
	return "Unrecognised";
}

string ToStrHelper<false, spv::MemoryModel>::Get(const spv::MemoryModel &el)
{
	switch(el)
	{
		case spv::MemoryModelSimple:   return "Simple";
		case spv::MemoryModelGLSL450:  return "GLSL450";
		case spv::MemoryModelOpenCL12: return "OpenCL12";
		case spv::MemoryModelOpenCL20: return "OpenCL20";
		case spv::MemoryModelOpenCL21: return "OpenCL21";
		default: break;
	}
	
	return "Unrecognised";
}

string ToStrHelper<false, spv::ExecutionModel>::Get(const spv::ExecutionModel &el)
{
	switch(el)
	{
		case spv::ExecutionModelVertex:    return "Vertex Shader";
		case spv::ExecutionModelTessellationControl: return "Tess. Control Shader";
		case spv::ExecutionModelTessellationEvaluation: return "Tess. Eval Shader";
		case spv::ExecutionModelGeometry:  return "Geometry Shader";
		case spv::ExecutionModelFragment:  return "Fragment Shader";
		case spv::ExecutionModelGLCompute: return "Compute Shader";
		case spv::ExecutionModelKernel:    return "Kernel";
		default: break;
	}
	
	return "Unrecognised";
}
