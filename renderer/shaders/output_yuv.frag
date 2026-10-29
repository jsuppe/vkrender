#version 450
layout(set=0,binding=0,std430) readonly buffer Source {uint pixels[];} src;
layout(push_constant) uniform Params {uint sw;uint sh;uint dw;uint dh;uint bgra;uint format;} p;
layout(location=0) out vec4 color;
ivec3 rgb(uint x,uint y){uint v=src.pixels[(((2*y+1)*p.sh)/(2*p.dh))*p.sw+((2*x+1)*p.sw)/(2*p.dw)];ivec3 c=ivec3(v&255,(v>>8)&255,(v>>16)&255);return p.bgra!=0?c.bgr:c;}
void main(){uvec2 xy=uvec2(gl_FragCoord.xy);ivec3 c=rgb(xy.x,xy.y);int y=16+((47*c.r+157*c.g+16*c.b+128)>>8);xy&=~uvec2(1);c=rgb(xy.x,xy.y)+rgb(xy.x+1,xy.y)+rgb(xy.x,xy.y+1)+rgb(xy.x+1,xy.y+1);int u=128+((-26*c.r-87*c.g+112*c.b+512)>>10);int v=128+((112*c.r-102*c.g-10*c.b+512)>>10);color=vec4(v,y,u,255)/255.;}
