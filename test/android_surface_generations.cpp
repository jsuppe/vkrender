// Shared scene fixture for Binder release/reacquire: old generation remains
// visible during exit while the same logical UID/slot gets a distinct tile.
#include "scene_layout.h"
#include <cstdio>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c); return 1; } } while(0)
int main() {
    vkr::ExternalVideoPool pool;
    pool.slots[10].width=1920; pool.slots[10].height=1080;
    pool.slots[20].width=1280; pool.slots[20].height=720;
    vkr::SceneMotion motion;
    std::vector<std::pair<uint64_t,int64_t>> cache;
    std::vector<vkr::Mat4> models; std::vector<int64_t> faces;
    auto scene=[&](std::vector<int64_t> slots,std::vector<uint64_t> generations) {
        vkr::buildScene(vkr::SceneLayout::Presentation,0,16.f/9,1./60,
                        slots,generations,pool,motion,cache,models,faces);
    };
    for(int i=0;i<180;i++)scene({10},{1});
    scene({20},{2});
    CHECK(motion.has(1)&&motion.has(2));
    CHECK(std::find(faces.begin(),faces.end(),10)!=faces.end());
    CHECK(std::find(faces.begin(),faces.end(),20)!=faces.end());
    for(int i=0;i<180;i++)scene({20},{2});
    CHECK(!motion.has(1)&&motion.has(2));
    CHECK(std::find(faces.begin(),faces.end(),10)==faces.end());
    for(int i=0;i<180;i++)scene({},{});
    CHECK(motion.count()==0&&cache.empty()&&models.empty());
    puts("Binder generation scene: concurrent old departure/new arrival and last-stream exit passed");
}
