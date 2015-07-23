#pragma once

#include "InputModule.h"
#include <vector>
#include "Rtypes.h"

class TClonesArray;
class TTree;
class PParticle;

namespace ant {
namespace input {

class PlutoInput: public BaseInputModule {
public:
    using PParticleVector = std::vector<const PParticle*>;

protected:
    TTree*          data = nullptr;
    TClonesArray*   PlutoMCTrue = nullptr;
    Long64_t        plutoID = 0;
    Long64_t        plutoRandomID = 0;

    PParticleVector particles;
public:

    PlutoInput();
    virtual ~PlutoInput();

    bool SetupBranches(TreeRequestManager&& input_files);
    void GetEntry();

    const PParticleVector&  Particles()   const { return particles; }
    Long64_t                GetPlutoID()  const { return plutoID; }
    Long64_t                GetRandomID() const { return plutoRandomID; }

    PlutoInput(const PlutoInput&) = delete;
    PlutoInput& operator= (const PlutoInput&) = delete;
};
}
}
