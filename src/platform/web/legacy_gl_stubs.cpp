// Issue #67. The fixed-function OpenGL calls the Glide backend still makes,
// which WebGL does not have: definitions so the web build links, each of which
// stops the run with a line if it is ever reached.
//
// They are not reached in phase 2: under Node there is no SDL video, so the
// Glide backend runs as its dummy and issues no GL call. Phase 3 moves the
// renderer to the GLES 3 / WebGL2 subset and this file goes with it
// (design #64). They stop rather than do nothing for the reason in this
// directory's README: a renderer that silently draws nothing is a state that
// looks like it works.

#if defined(__EMSCRIPTEN__)

#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <cstdlib>

namespace
{

[[noreturn]] void FixedFunction(const char* name)
{
    std::fprintf(stderr,
                 "[repiu-web] %s is fixed-function OpenGL, which WebGL does not "
                 "have; the Glide renderer moves to WebGL2 in phase 3\n",
                 name);
    std::abort();
}

}  // namespace

extern "C" {

void glAlphaFunc(GLenum, GLclampf) { FixedFunction("glAlphaFunc"); }
void glColor4f(GLfloat, GLfloat, GLfloat, GLfloat) { FixedFunction("glColor4f"); }
void glDrawBuffer(GLenum) { FixedFunction("glDrawBuffer"); }
void glEnd(void) { FixedFunction("glEnd"); }
void glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) { FixedFunction("glOrtho"); }
void glPointSize(GLfloat) { FixedFunction("glPointSize"); }
void glPopAttrib(void) { FixedFunction("glPopAttrib"); }
void glPushAttrib(GLbitfield) { FixedFunction("glPushAttrib"); }
void glReadBuffer(GLenum) { FixedFunction("glReadBuffer"); }
void glTexCoord4f(GLfloat, GLfloat, GLfloat, GLfloat) { FixedFunction("glTexCoord4f"); }
void glVertex3f(GLfloat, GLfloat, GLfloat) { FixedFunction("glVertex3f"); }

}  // extern "C"

#endif  // __EMSCRIPTEN__
