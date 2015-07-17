#ifndef ACCELERATOR_HH
#define ACCELERATOR_HH

#include <cctk.h>
#include <cassert>
#include <vector>
#include <iostream>


namespace Accelerator {
  
  using namespace std;
  
  
  
  class id_t {
  public:
    struct ls_t { int vi, rl, tl; };
  private:
    vector<vector<vector<int> > > ids;
    vector<ls_t> lss;
  public:
    bool id_exists(int vi, int rl, int tl) const
    {
      if (int(ids.size()) <= vi) return false;
      if (int(ids.at(vi).size()) <= rl)  return false;
      if (int(ids.at(vi).at(rl).size()) <= tl) return false;
      return true;
    }
    int get_existing_id(int vi, int rl, int tl) const
    {
      return ids.at(vi).at(rl).at(tl);
    }
    int get_id(int vi, int rl, int tl)
    {
      if (int(ids.size()) <= vi) {
        ids.resize(CCTK_NumVars());
      }
      if (int(ids.at(vi).size()) <= rl) {
        ids.at(vi).resize(GetRefinementLevels(NULL));
      }
      if (int(ids.at(vi).at(rl).size()) <= tl) {
        ids.at(vi).at(rl).reserve(tl+1);
      }
      while (int(ids.at(vi).at(rl).size()) <= tl) {
        ids.at(vi).at(rl).push_back(lss.size());
        ls_t const ls = { vi, rl, int(ids.at(vi).at(rl).size()) };
        lss.push_back(ls);
      }
      return get_existing_id(vi, rl, tl);
    }
    ls_t const& resolve_id(int id) const
    {
      return lss.at(id);
    }
  };
  
  
  
  class vars_t {
    vector<CCTK_INT> vis;
    vector<CCTK_INT> rls;
    vector<CCTK_INT> tls;
  public:
    void push_back(int vi, int rl, int tl)
    {
      vis.push_back(vi);
      rls.push_back(rl);
      tls.push_back(tl);
    }
    CCTK_INT const *vi_ptr() const { return &vis.front(); }
    CCTK_INT const *rl_ptr() const { return &rls.front(); }
    CCTK_INT const *tl_ptr() const { return &tls.front(); }
    int nvars() const { return vis.size(); }
  };
  
  struct mem_t;

  struct bool_field {
    mem_t *m;
    bool f;
    bool_field(mem_t *m_,bool f_) : m(m_), f(f_) {}
    void operator=(bool b);
    operator bool() const {
      return f;
    }
  };
  enum bfield { host_field=0, device_field=1 };
  // Allow us to track when changes are made to mem_t
  struct mem_t {
    bool_field host_valid, device_valid;
    mem_t() : host_valid(this,true), device_valid(this,false) {}
    mem_t(bool h,bool d) : host_valid(this,h), device_valid(this,d) {}
    void check() {
      assert(host_valid || device_valid);
    }
    bool_field& operator[](int n) {
      assert(n == 0 || n == 1);
      if(n == 0)
        return host_valid;
      else
        return device_valid;
    }
  };
  inline void bool_field::operator=(bool b) {
    //assert(m->host_valid || m->device_valid);
    f = b;
    assert(m != 0);
    //assert(m->host_valid || m->device_valid);
  }
  
  class device_t {
    vector<vector<vector<mem_t> > > mems; // [vi][rl][tl]
    void ensure_reflevel(int vi, int rl)
    {
      if (rl >= int(mems.at(vi).size())) {
        // Allocate space for all refinement levels, so that we don't
        // have to reallocate too often
        int const rls = GetRefinementLevels(NULL);
        mems.at(vi).resize(rls);
      }
    }
  public:
    device_t(): mems(CCTK_NumVars()) {}
    int ntimelevels(int vi, int rl) const
    {
      if (rl < int(mems.at(vi).size())) {
        return mems.at(vi).at(rl).size();
      } else {
        return 0;
      }
    }
    void create_timelevel(int vi, int rl, int tl, mem_t const& m)
    {
      ensure_reflevel(vi, rl);
      // We can only create the next missing timelevel
      assert(int(mems.at(vi).at(rl).size()) == tl);
      mems.at(vi).at(rl).resize(tl+1, m);
    }
    mem_t& mem(int vi, int rl, int tl)
    {
      ensure_reflevel(vi, rl);
      return mems.at(vi).at(rl).at(tl);
    }

    mem_t *get(int vi,int rl,int tl) {
        const char *var_name = CCTK_VarName(vi);
        if(var_name == 0)
          return 0;
        if(vi >= mems.size())
          return 0;
        if(rl >= mems[vi].size())
          return 0;
        if(tl >= mems[vi][rl].size())
          return 0;
        return &mems[vi][rl][tl];
    }

    void dump() {
      std::cout << "Accel:dump {{{" << std::endl;
      for(int vi=0;vi<mems.size();vi++) {
        const char *var_name = CCTK_VarName(vi);
        if(var_name == 0)
          continue;
        for(int rl=0;rl<mems[vi].size();rl++) {
          for(int tl=0;tl<mems[vi][rl].size();tl++) {
            mem_t& m = mems[vi][rl][tl];
            std::cout << "::dump " << var_name << "(" << vi << "):" << rl << ":" << tl << "={" << m.host_valid << "," << m.device_valid << "}" << std::endl;
          }
        }
      }
      std::cout << "}}} Accel:dump" << std::endl;
    }
  };
  
  extern device_t *device;
  
} // namespace Accelerator

extern "C" void Accel_varlocation(int vi,int rl,int tl,char *data);
extern "C" void Accel_dump();

#endif  // #ifndef ACCELERATOR_HH
