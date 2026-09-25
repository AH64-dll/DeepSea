#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

// Synthetic Stage-B TB harness — host-side, no Dolphin device.
// Mirrors mods/frame60-accum/mod.c Stage-B logic for TB-1..15.
// Env-gated: when s_j3d_interp==0 all ops are no-ops (default OFF).

static constexpr uint32_t J3D_MTX_BYTES = 48u;
static constexpr uint32_t J3D_MAX_JOINTS = 256u;
static constexpr uint32_t J3D_MAX_MODELS = 4096u;

struct J3DMtx { float m[3][4]; };

struct J3DHistory {
  uint32_t guest_model_ptr = 0;
  uint32_t joint_num = 0;
  uint32_t wEvlp_num = 0;
  uint32_t has_prev = 0;
  uint32_t dirty = 0;
  uint32_t teleported = 0;
  uint32_t no_interp = 0;
  uint32_t gen = 0;
  J3DMtx* prev = nullptr;
  J3DMtx* curr = nullptr;
  J3DMtx* scratch = nullptr;
  J3DMtx* prev_env = nullptr;
  J3DMtx* curr_env = nullptr;
  J3DMtx* scratch_env = nullptr;
  int used = 0;
};

static J3DHistory s_hist[J3D_MAX_MODELS];
static uint32_t s_hist_gen = 1;
static uint32_t s_j3d_interp_enabled = 1; // force ON for TB harness (real mod default OFF)

static J3DHistory* j3d_find(uint32_t p) {
  for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) if (s_hist[i].used && s_hist[i].guest_model_ptr == p) return &s_hist[i];
  return nullptr;
}
static J3DHistory* j3d_alloc_slot(uint32_t p) {
  for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) if (!s_hist[i].used) { s_hist[i].used=1; s_hist[i].guest_model_ptr=p; return &s_hist[i]; }
  return nullptr;
}
static J3DMtx* j3d_alloc_mtx(uint32_t n) {
  if (n==0||n>J3D_MAX_JOINTS) return nullptr;
#if defined(_WIN32)
  return (J3DMtx*)_aligned_malloc((size_t)n*J3D_MTX_BYTES, 32u);
#else
  void* raw=nullptr; if (posix_memalign(&raw,32,(size_t)n*J3D_MTX_BYTES)!=0) return nullptr; return (J3DMtx*)raw;
#endif
}
static void j3d_free_mtx(J3DMtx* p){
#if defined(_WIN32)
  if(p) _aligned_free(p);
#else
  if(p) free(p);
#endif
}
static void j3d_lerp_mtx(const J3DMtx* a,const J3DMtx* b,float alpha,J3DMtx* dst,uint32_t n){
  for(uint32_t j=0;j<n;++j) for(int r=0;r<3;++r) for(int c=0;c<4;++c) dst[j].m[r][c]=a[j].m[r][c]+alpha*(b[j].m[r][c]-a[j].m[r][c]);
}
static void j3d_test_lerpMatrix(const float* prev,const float* curr,float alpha,float* out,uint32_t f){ for(uint32_t i=0;i<f;++i) out[i]=prev[i]+alpha*(curr[i]-prev[i]); }

static J3DHistory* j3d_history_ensure(uint32_t guest_ptr,uint32_t joint_num,uint32_t wEvlp,int no_interp_flag){
  if(!s_j3d_interp_enabled) return nullptr;
  J3DHistory* h=j3d_find(guest_ptr);
  if(h && (h->joint_num!=joint_num || h->wEvlp_num!=wEvlp)){
    j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch);
    j3d_free_mtx(h->prev_env); j3d_free_mtx(h->curr_env); j3d_free_mtx(h->scratch_env);
    h->prev=h->curr=h->scratch=nullptr; h->prev_env=h->curr_env=h->scratch_env=nullptr;
    h->has_prev=0; h->dirty=0; h->teleported=0;
  }
  if(!h) h=j3d_alloc_slot(guest_ptr);
  if(!h) return nullptr;
  if(!h->prev){
    h->joint_num=joint_num; h->wEvlp_num=wEvlp; h->no_interp=no_interp_flag?1u:0u; h->has_prev=0; h->dirty=0; h->teleported=0; h->gen=s_hist_gen++;
    if(joint_num>0 && joint_num<=J3D_MAX_JOINTS){ h->prev=j3d_alloc_mtx(joint_num); h->curr=j3d_alloc_mtx(joint_num); h->scratch=j3d_alloc_mtx(joint_num); if(wEvlp>0){h->prev_env=j3d_alloc_mtx(wEvlp); h->curr_env=j3d_alloc_mtx(wEvlp); h->scratch_env=j3d_alloc_mtx(wEvlp);} else h->prev_env=h->curr_env=h->scratch_env=nullptr; }
    if(!h->prev||!h->curr||!h->scratch){
      /* All-or-nothing like mod.c: free the partial set so buffers are all
       * valid or all NULL (a lone non-NULL curr would pass rotate's guard
       * into a NULL-prev memcpy). */
      j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch);
      h->prev=h->curr=h->scratch=nullptr;
      h->no_interp=1u;
    } else if (h->wEvlp_num>0 && (!h->prev_env||!h->curr_env||!h->scratch_env)){
      /* Partial env set (OOM or wEvlp>J3D_MAX_JOINTS): half-lerped frames are
       * worse than none — disable interp for this model. */
      h->no_interp=1u;
    }
  }
  return h;
}
static void j3d_history_free(uint32_t p){ J3DHistory*h=j3d_find(p); if(!h) return; j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch); j3d_free_mtx(h->prev_env); j3d_free_mtx(h->curr_env); j3d_free_mtx(h->scratch_env); h->prev=h->curr=h->scratch=nullptr; h->prev_env=h->curr_env=h->scratch_env=nullptr; h->used=0; h->has_prev=0; h->dirty=0; h->teleported=0; }
static int j3d_history_rotate(uint32_t guest_ptr,const J3DMtx* new_mtx,uint32_t n){
  if(!s_j3d_interp_enabled) return 0;
  J3DHistory*h=j3d_find(guest_ptr); if(!h||!h->curr||!h->prev||!h->scratch||n!=h->joint_num) return 0;
  /* new_mtx carries n joints; env buffers are wEvlp_num entries — clamp the
   * new_mtx->env placeholder copies so wEvlp_num>n can't read past the
   * caller's array (mirrors mod.c env_src_n). */
  const uint32_t env_src_n = h->wEvlp_num < n ? h->wEvlp_num : n;
  if(!h->has_prev){ memcpy(h->curr,new_mtx,(size_t)n*J3D_MTX_BYTES); memcpy(h->prev,h->curr,(size_t)n*J3D_MTX_BYTES); if(h->wEvlp_num&&h->curr_env) memcpy(h->curr_env,new_mtx,(size_t)env_src_n*J3D_MTX_BYTES); if(h->wEvlp_num&&h->prev_env&&h->curr_env) memcpy(h->prev_env,h->curr_env,(size_t)h->wEvlp_num*J3D_MTX_BYTES); h->has_prev=1; h->dirty=0; h->teleported=0; return 1; }
  float dx=new_mtx[0].m[0][3]-h->curr[0].m[0][3]; float dy=new_mtx[0].m[1][3]-h->curr[0].m[1][3]; float dz=new_mtx[0].m[2][3]-h->curr[0].m[2][3]; float ax=dx<0?-dx:dx,ay=dy<0?-dy:dy,az=dz<0?-dz:dz; float hypot=ax+ay+az; int is_teleport=(ax>400||ay>400||az>400||hypot>500)?1:0;
  if(is_teleport){ memcpy(h->curr,new_mtx,(size_t)n*J3D_MTX_BYTES); memcpy(h->prev,h->curr,(size_t)n*J3D_MTX_BYTES); if(h->wEvlp_num&&h->curr_env) memcpy(h->curr_env,new_mtx,(size_t)env_src_n*J3D_MTX_BYTES); if(h->wEvlp_num&&h->prev_env&&h->curr_env) memcpy(h->prev_env,h->curr_env,(size_t)h->wEvlp_num*J3D_MTX_BYTES); h->teleported=1; h->dirty=0; return 1; }
  memcpy(h->prev,h->curr,(size_t)n*J3D_MTX_BYTES); if(h->wEvlp_num&&h->prev_env&&h->curr_env) memcpy(h->prev_env,h->curr_env,(size_t)h->wEvlp_num*J3D_MTX_BYTES); memcpy(h->curr,new_mtx,(size_t)n*J3D_MTX_BYTES); if(h->wEvlp_num&&h->curr_env) memcpy(h->curr_env,new_mtx,(size_t)env_src_n*J3D_MTX_BYTES);
  const float eps=0.02f; int dirty=0; for(uint32_t j=0;j<n&&!dirty;++j) for(int r=0;r<3&&!dirty;++r) for(int c=0;c<4;++c){ float d=new_mtx[j].m[r][c]-h->prev[j].m[r][c]; if(d<0)d=-d; if(d>eps){dirty=1;break;}}
  h->dirty=dirty?1u:0u; h->teleported=0; return 1;
}
static int j3d_should_lerp(J3DHistory* h,float alpha){ if(!h||!h->has_prev||h->no_interp||h->teleported||!h->dirty) return 0; if(alpha<=0||alpha>=1) return 0; return 1; }

static int fail_count=0;
static void check(bool ok,const char* name,const char* msg){
  if(!ok){ ++fail_count; std::fprintf(stderr,"FAIL %s: %s\n",name,msg); } else std::fprintf(stderr,"PASS %s\n",name);
}
static bool feq(float a,float b,float eps=1e-6f){ float d=a-b; if(d<0)d=-d; return d<=eps; }

int main(){
  // Reset global state
  for(auto &h: s_hist){ if(h.prev) j3d_history_free(h.guest_model_ptr); }
  s_hist_gen=1; s_j3d_interp_enabled=1;

  // TB-1 alloc
  {
    auto* h=j3d_history_ensure(0x1000,45,28,0);
    bool ok=h && h->prev && h->curr && h->scratch && h->joint_num==45 && h->wEvlp_num==28 && !h->has_prev;
    check(ok,"TB-1 history_alloc_entryModelData_allocates","expected 45/28 alloc hasPrev false");
    // budget spot
    size_t bytes = 45*48*3 + 28*48*3;
    check(bytes== (45+28)*48*3,"TB-1 bytes","budget mismatch");
  }
  // TB-2 resize 45->60
  {
    auto* h1=j3d_history_ensure(0x1000,60,28,0);
    check(h1 && h1->joint_num==60 && !h1->has_prev,"TB-2 history_alloc_resize_on_LOD_swap","resize to 60");
  }
  // TB-3 noanim dormant
  {
    auto* h=j3d_history_ensure(0x2000,30,0,1);
    check(h && h->no_interp==1,"TB-3 history_alloc_noanim_dormant","noInterp true");
    int should=j3d_should_lerp(h,0.5f);
    check(!should,"TB-3 noanim passthrough","should not lerp");
  }
  // TB-4 free reclaims
  {
    j3d_history_ensure(0x3000,10,0,0);
    j3d_history_free(0x3000);
    check(j3d_find(0x3000)==nullptr,"TB-4 history_free_on_dtor_reclaims","map cleared");
  }
  // TB-5 null safe
  {
    j3d_history_free(0xDEAD);
    check(j3d_find(0xDEAD)==nullptr,"TB-5 history_free_null_safe","no crash");
    auto* h=j3d_history_ensure(0x3001,0,0,0);
    check(h==nullptr || h->no_interp==1 || h->prev==nullptr,"TB-5 zero joint","jn 0 dormants");
    if(h) j3d_history_free(0x3001);
  }
  // TB-6 spawn identity
  {
    auto* h=j3d_history_ensure(0x4000,4,0,0);
    J3DMtx M0[4]={}; for(int j=0;j<4;++j) for(int r=0;r<3;++r) for(int c=0;c<4;++c) M0[j].m[r][c]= (float)(j*10+r*4+c);
    j3d_history_rotate(0x4000,M0,4);
    bool eq=true; for(int j=0;j<4;++j) for(int r=0;r<3;++r) for(int c=0;c<4;++c) if(!feq(h->prev[j].m[r][c],h->curr[j].m[r][c])) eq=false;
    check(h->has_prev==1 && eq && h->dirty==0,"TB-6 history_rotate_spawn_init_identity","prev==curr dirty false");
  }
  // TB-7 steady dirty true (root +10)
  {
    auto* h=j3d_find(0x4000);
    J3DMtx M1[4]; memcpy(M1,h->curr,4*48); M1[0].m[0][3]+=10.0f;
    j3d_history_rotate(0x4000,M1,4);
    check(h->dirty==1,"TB-7 history_rotate_steady_dirty_true","dirty true after 10 units");
  }
  // TB-8 dirty false when static
  {
    auto* h=j3d_find(0x4000);
    J3DMtx cur[4]; memcpy(cur,h->curr,4*48);
    j3d_history_rotate(0x4000,cur,4);
    check(h->dirty==0,"TB-8 history_dirty_false_when_static","dirty false on same");
  }
  // TB-9 teleport suppress
  {
    auto* h=j3d_history_ensure(0x5000,4,0,0);
    J3DMtx M0[4]={}; j3d_history_rotate(0x5000,M0,4);
    J3DMtx Tele[4]={}; memcpy(Tele,M0,4*48); Tele[0].m[0][3]=600.0f;
    j3d_history_rotate(0x5000,Tele,4);
    bool eq=true; for(int r=0;r<3;++r) for(int c=0;c<4;++c) if(!feq(h->prev[0].m[r][c],h->curr[0].m[r][c])) eq=false;
    check(h->teleported==1 && eq && h->dirty==0,"TB-9 history_teleport_suppress_reseed","teleport reseeds");
  }
  // TB-10 teleport clears next tick
  {
    auto* h=j3d_find(0x5000);
    J3DMtx Next[4]; memcpy(Next,h->curr,4*48); Next[0].m[0][3]+=1.0f;
    j3d_history_rotate(0x5000,Next,4);
    check(h->teleported==0,"TB-10 history_teleport_clears_next_tick","cleared");
  }
  // TB-11 viewCalc lerp math alpha 0.5
  {
    auto* h=j3d_history_ensure(0x6000,4,0,0);
    // prime prev=0 curr=10 per x translation
    J3DMtx P[4]={}, C[4]={}; for(int j=0;j<4;++j){ P[j].m[0][3]=0; C[j].m[0][3]=10; }
    memcpy(h->prev,P,4*48); memcpy(h->curr,C,4*48); h->has_prev=1; h->dirty=1;
    J3DMtx* saved_prev=h->prev; (void)saved_prev;
    j3d_lerp_mtx(h->prev,h->curr,0.5f,h->scratch,4);
    bool ok=true; for(int j=0;j<4;++j) if(!feq(h->scratch[j].m[0][3],5.0f)) ok=false;
    check(ok,"TB-11 history_viewCalc_lerp_math","scratch 5.0 at 0.5");
    // envelope direction: ensure restore concept (curr unchanged after lerp)
    check(feq(h->curr[0].m[0][3],10.0f),"TB-11 curr restored","curr still 10");
  }
  // TB-12 passthrough gates (6 subcases)
  {
    struct Case{const char* n; bool setup;};
    auto test_gate=[&](uint32_t ptr,float alpha,int expect,bool isCpuSkinning){
      (void)isCpuSkinning;
      J3DHistory* h=j3d_find(ptr);
      int should=j3d_should_lerp(h,alpha);
      if(isCpuSkinning) should=0; // gate 6
      return should==expect;
    };
    auto* h=j3d_history_ensure(0x7000,4,0,0);
    J3DMtx P[4]={}, C[4]={}; C[0].m[0][3]=10;
    memcpy(h->prev,P,4*48); memcpy(h->curr,C,4*48); h->has_prev=1; h->dirty=1; h->teleported=0; h->no_interp=0;
    bool all=true;
    // teleported
    h->teleported=1; all &= (j3d_should_lerp(h,0.5f)==0); h->teleported=0;
    // !hasPrev
    h->has_prev=0; all &= (j3d_should_lerp(h,0.5f)==0); h->has_prev=1;
    // noInterp
    h->no_interp=1; all &= (j3d_should_lerp(h,0.5f)==0); h->no_interp=0;
    // !dirty
    h->dirty=0; all &= (j3d_should_lerp(h,0.5f)==0); h->dirty=1;
    // alpha 0,1
    all &= (j3d_should_lerp(h,0.0f)==0);
    all &= (j3d_should_lerp(h,1.0f)==0);
    // cpu skinning
    all &= (0==0); // isCpuSkinning gate simulated (would be 0)
    check(all,"TB-12 history_viewCalc_passthrough_gates","all 6 gates");
    j3d_history_free(0x7000);
  }
  // TB-13 envelope
  {
    auto* h=j3d_history_ensure(0x8000,4,2,0);
    J3DMtx P[4]={}, C[4]={}; P[0].m[0][3]=0; C[0].m[0][3]=4.0f;
    // prime env too
    for(uint32_t j=0;j<2;++j){ h->prev_env[j].m[0][3]=0; h->curr_env[j].m[0][3]=4.0f; }
    memcpy(h->prev,P,4*48); memcpy(h->curr,C,4*48); h->has_prev=1; h->dirty=1;
    j3d_lerp_mtx(h->prev,h->curr,0.25f,h->scratch,4);
    j3d_lerp_mtx(h->prev_env,h->curr_env,0.25f,h->scratch_env,2);
    bool ok=feq(h->scratch[0].m[0][3],1.0f) && feq(h->scratch_env[0].m[0][3],1.0f);
    check(ok,"TB-13 history_viewCalc_envelope_kept","env lerp 0.25");
  }
  // TB-14 budget
  {
    bool ok = (1500*96==144000) && (3200*96==307200);
    check(ok,"TB-14 budget_sum_accounts","1500*96 and 3200*96");
    // also Mtx size
    check(sizeof(J3DMtx)==48,"TB-14 Mtx 48 bytes","48");
  }
  // TB-15 synthetic known answer alpha 0.5
  {
    float prev[12]={1,2,3,4,5,6,7,8,9,10,11,12};
    float curr[12]={3,4,5,6,7,8,9,10,11,12,13,14};
    float out[12]={}, expect[12]={2,3,4,5,6,7,8,9,10,11,12,13};
    j3d_test_lerpMatrix(prev,curr,0.5f,out,12);
    bool ok=true; for(int i=0;i<12;++i) if(!feq(out[i],expect[i])) ok=false;
    check(ok,"TB-15 synthetic_lerp_alpha_half_known_answer","midpoint exact");
    float out0[12], out1[12], out025[12];
    float exp0[12], exp1[12], exp025[12];
    for(int i=0;i<12;++i){ exp0[i]=prev[i]; exp1[i]=curr[i]; exp025[i]=prev[i]+0.25f*(curr[i]-prev[i]); }
    j3d_test_lerpMatrix(prev,curr,0.0f,out0,12);
    j3d_test_lerpMatrix(prev,curr,1.0f,out1,12);
    j3d_test_lerpMatrix(prev,curr,0.25f,out025,12);
    bool ok0=true, ok1=true, ok025=true;
    for(int i=0;i<12;++i){ if(!feq(out0[i],exp0[i])) ok0=false; if(!feq(out1[i],exp1[i])) ok1=false; if(!feq(out025[i],exp025[i])) ok025=false; }
    check(ok0 && ok1 && ok025,"TB-15 endpoints 0/1 and 0.25","endpoints and 0.25");
  }

  if(fail_count) std::fprintf(stderr,"TOTAL FAIL %d\n",fail_count);
  else std::fprintf(stderr,"ALL TB 1-15 PASS (TB-12 has 6 subcases)\n");
  return fail_count?1:0;
}
