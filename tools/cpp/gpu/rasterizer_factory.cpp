// rasterizer_factory.cpp — central factory. Picks a backend by enum, guarded so
// headless/CI builds never require a host GL context. See GPU_TODO.md §6.
#include "igpu_rasterizer.h"

namespace zeebo::gpu {

std::unique_ptr<IGpuRasterizer> create_rasterizer(Backend b){
#ifdef ZEEBO_GL_HOST
    if(b==Backend::GlHost) return make_gl_host_rasterizer();
#endif
    (void)b;                       // GlHost requested without GL support -> soft
    return make_soft_rasterizer(); // always-available correctness backend
}

} // namespace zeebo::gpu
