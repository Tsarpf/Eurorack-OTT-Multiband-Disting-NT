// Offline float32 stereo renderer using the host-test NT API stubs.
#define main ott_test_main
#include "../test_ott.cpp"
#undef main
#include <fstream>
int main(int argc,char** argv) {
 if(argc!=5) { std::cerr << "usage: ott_render input.f32 output.f32 depth block_frames\n"; return 1; }
 int depth=atoi(argv[3]),N=atoi(argv[4]);
 if(depth<0||depth>100||N<4||N>64||N%4) return 2;
 std::ifstream in(argv[1],std::ios::binary); std::ofstream out(argv[2],std::ios::binary);
 if(!in || !out) return 3;
 auto h=makeOtt();
 // Explicitly bind the returned host's storage independently of NRVO.
 h.alg->v=h.v; h.alg->vIncludingCommon=h.common;
 h.v[kGlobalDepth]=depth; factory.parameterChanged(h.alg,kGlobalDepth);
 float interleaved[128]={},bus[256]={};
 while(in.read((char*)interleaved,2*N*sizeof(float)) || in.gcount()) {
  if(in.gcount() % (2*sizeof(float))) return 4;
  int count=in.gcount()/(2*sizeof(float));
  for(int i=0;i<N;++i) {bus[i]=i<count?interleaved[2*i]:0;bus[N+i]=i<count?interleaved[2*i+1]:0;}
  factory.step(h.alg,bus,N/4);
  for(int i=0;i<count;++i){interleaved[2*i]=bus[2*N+i];interleaved[2*i+1]=bus[3*N+i];}
  out.write((char*)interleaved,2*count*sizeof(float));
 }
}
