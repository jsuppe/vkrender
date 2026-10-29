#include "scene_layout.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 vkr::ExternalVideoPool pool;
 std::vector<int64_t> slots={10,20};std::vector<uint64_t> ids={1,2};
 pool.slots[10].width=1920;pool.slots[10].height=1080;
 pool.slots[20].width=720;pool.slots[20].height=1280;
 for(float aspect:{0.5625f,1.77778f,3.5f})for(auto layout:{vkr::SceneLayout::TwoCubes,vkr::SceneLayout::SingleCube,vkr::SceneLayout::Wall,vkr::SceneLayout::Presentation}){
  vkr::SceneMotion motion;std::vector<std::pair<uint64_t,int64_t>> cache;
  std::vector<vkr::Mat4> models;std::vector<int64_t> faces;
  for(int i=0;i<180;i++)vkr::buildScene(layout,1,aspect,1.0/60,slots,ids,pool,motion,cache,models,faces);
  CHECK(models.size()==(layout==vkr::SceneLayout::SingleCube?1:2));CHECK(faces.size()==models.size()*6);
  for(auto&m:models)for(float f:m.m)CHECK(std::isfinite(f));
  if(layout==vkr::SceneLayout::Wall||layout==vkr::SceneLayout::Presentation){CHECK(faces[0]==10);CHECK(faces[6]==20);}
  if(layout==vkr::SceneLayout::Presentation){
   CHECK(std::abs(models[0].m[0]/models[0].m[5]-1920.0f/1080)<.001f);
   CHECK(std::abs(models[1].m[0]/models[1].m[5]-720.0f/1280)<.001f);
  }
 }
 puts("Shared desktop/Android layouts: all four layouts, portrait/wide canvas, mixed source aspect ratios passed");
}
