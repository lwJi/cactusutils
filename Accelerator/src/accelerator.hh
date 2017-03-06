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
  
  
  
  struct mem_t {
    enum memtype_t { host_is_valid, device_is_valid };
    void set_host_valid(bool b) {
      host_valid = b;
      diag();
    }
    void set_device_valid(bool b) {
      device_valid = b;
      diag();
    }
    bool get_host_valid() const {
      return host_valid;
    }
    bool get_device_valid() const {
      return device_valid;
    }
    bool get(memtype_t m) const {
      if(m == host_is_valid)
        return host_valid;
      else
        return device_valid;
    }
    void set(memtype_t m,bool b) {
      if(m == host_is_valid)
        set_host_valid(b);
      else
        set_device_valid(b);
      diag();
    }
    mem_t() : vi(-1), tl(-1), rl(-1), host_valid(false), device_valid(false) {}
    mem_t(int vi_,int rl_,int tl_,bool h,bool d) : vi(vi_), tl(tl_), rl(rl_), host_valid(h), device_valid(d) {}
    int vi, tl, rl;
  private:
    bool host_valid, device_valid;
    /**
     * Uncomment the line of code below to flood the output
     * with diagnostics about when the mem_t structure is
     * changed.
     */
    void diag() {
      // std::cout << "mem_t:" << CCTK_VarName(vi) << ":rl=" << rl << ":tl=" << tl << " hd=" << host_valid << device_valid << std::endl;
    }
  };
  
  class device_t {
    vector<vector<vector<mem_t> > > mems; // [vi][rl][tl]
    void ensure_reflevel(int vi, int rl)
    {
      // rl can be less than zero if this
      // routine is called at an inappropriate
      // time in the schedule.
      assert(rl >= 0);
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
    void ensure_timelevel(int vi, int rl, int tl) {
      ensure_reflevel(vi, rl);
      while(tl >= int(mems.at(vi).at(rl).size())) {
        mem_t m(vi,rl,mems.at(vi).at(rl).size(),false,false);
        mems.at(vi).at(rl).push_back(m);
      }
    }
    void create_timelevel(int vi, int rl, int tl, mem_t const& m)
    {
      ensure_reflevel(vi, rl);
      // We can only create the next missing timelevel
      assert(int(mems.at(vi).at(rl).size()) == tl);
      mems.at(vi).at(rl).resize(tl+1, m);
      mem_t& mm = mems.at(vi).at(rl).at(tl);
      mm.vi = vi;
      mm.rl = rl;
      mm.tl = tl;
    }
    bool has_mem(int vi,int rl,int tl)
    {
      if(mems.size() <= vi) return false;
      if(mems.at(vi).size() <= rl) return false;
      if(mems.at(vi).at(rl).size() <= tl) return false;
      return true;
    }
    mem_t& mem(int vi, int rl, int tl)
    {
      std::string vn = CCTK_VarName(vi);
      ensure_reflevel(vi, rl);
      assert(has_mem(vi,rl,tl));

      mem_t& m = mems.at(vi).at(rl).at(tl);

      // assert mem_t was properly initialized
      assert(m.vi != -1);
      assert(m.rl != -1);
      assert(m.tl != -1);

      // assert we have the mem_t structure we think we have
      assert(m.vi == vi);
      assert(m.rl == rl);
      assert(m.tl == tl);

      return m;
    }
  };
  
  extern device_t *device;
  
} // namespace Accelerator

#endif  // #ifndef ACCELERATOR_HH
