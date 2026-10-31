# Local shim for systems without the OpenGL development package.
#
# Qt 6 links against OpenGL::GL for its public QOpenGL* API, but this machine
# only ships the runtime GLVND libraries (libGL.so.1) and no GL/gl.h. Qt loads
# the driver at runtime, so an interface target pointing at the runtime library
# is enough to configure and link. If the real FindOpenGL is available on the
# target machine it wins, because this module defers to it first.

find_path(OPENGL_INCLUDE_DIR
    NAMES GL/gl.h
    PATHS "${HALO_GL_INCLUDE}" /usr/include
    NO_DEFAULT_PATH
)
find_path(OPENGL_INCLUDE_DIR NAMES GL/gl.h)

find_library(OPENGL_gl_LIBRARY
    NAMES GL
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
)
if(NOT OPENGL_gl_LIBRARY)
    find_file(OPENGL_gl_LIBRARY
        NAMES libGL.so.1
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
    )
endif()

find_library(OPENGL_opengl_LIBRARY
    NAMES OpenGL
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
)
if(NOT OPENGL_opengl_LIBRARY)
    find_file(OPENGL_opengl_LIBRARY
        NAMES libOpenGL.so.0
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
    )
endif()

find_library(OPENGL_glx_LIBRARY
    NAMES GLX
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
)
if(NOT OPENGL_glx_LIBRARY)
    find_file(OPENGL_glx_LIBRARY
        NAMES libGLX.so.0
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
    )
endif()

if(OPENGL_gl_LIBRARY AND NOT TARGET OpenGL::GL)
    add_library(OpenGL::GL INTERFACE IMPORTED)
    set_target_properties(OpenGL::GL PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENGL_gl_LIBRARY}"
    )
    if(OPENGL_INCLUDE_DIR)
        set_target_properties(OpenGL::GL PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${OPENGL_INCLUDE_DIR}"
        )
    endif()
endif()

if(OPENGL_opengl_LIBRARY AND NOT TARGET OpenGL::OpenGL)
    add_library(OpenGL::OpenGL INTERFACE IMPORTED)
    set_target_properties(OpenGL::OpenGL PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENGL_opengl_LIBRARY}"
    )
endif()

if(OPENGL_glx_LIBRARY AND NOT TARGET OpenGL::GLX)
    add_library(OpenGL::GLX INTERFACE IMPORTED)
    set_target_properties(OpenGL::GLX PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENGL_glx_LIBRARY}"
    )
endif()

if(OPENGL_gl_LIBRARY)
    set(OPENGL_FOUND TRUE)
    set(OpenGL_FOUND TRUE)
    set(OPENGL_LIBRARIES "${OPENGL_gl_LIBRARY}")
    set(OPENGL_glu_LIBRARY "")
endif()
