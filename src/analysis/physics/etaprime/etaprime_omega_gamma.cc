#include "etaprime_omega_gamma.h"

#include "plot/root_draw.h"

#include "utils/particle_tools.h"
#include "utils/matcher.h"
#include "utils/combinatorics.h"
#include "analysis/utils/MCFakeReconstructed.h"

#include "expconfig/ExpConfig.h"

#include "base/Logger.h"
#include "base/std_ext/math.h"
#include "base/std_ext/vector.h"
#include "base/std_ext/misc.h"

#include <TTree.h>

#include <limits>


using namespace std;
using namespace ant;
using namespace ant::analysis;
using namespace ant::analysis::physics;

const ParticleTypeTree EtapOmegaG::ptreeSignal = ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::EtaPrime_gOmega_ggPi0_4g);
const ParticleTypeTree EtapOmegaG::ptreeReference = ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::EtaPrime_2g);




APLCON::Fit_Settings_t EtapOmegaG::MakeFitSettings(unsigned max_iterations)
{
    auto settings = APLCON::Fit_Settings_t::Default;
    settings.MaxIterations = max_iterations;
    //    settings.ConstraintAccuracy = 1.0e-3;
    //    settings.Chi2Accuracy = 1.0e-2;
    //    settings.DebugLevel = 5;
    return settings;
}

EtapOmegaG::EtapOmegaG(const string& name, OptionsPtr opts) :
    Physics(name, opts),
    FlagWolfgang(opts->Get<bool>("Wolfgang", false)),
    fitparams(// use FitterSergey as default
              make_shared<utils::UncertaintyModels::FitterSergey>(),
              true, // flag to enable z vertex
              3.0  // Z_vertex_sigma, =0 means unmeasured
              ),
    mc_smear(opts->Get<bool>("MCSmear", false) ?
                 std_ext::make_unique<utils::MCSmear>(
                     utils::UncertaintyModels::Interpolated::makeAndLoad(
                         // use Adlarson as default (30% version of Oli is maybe better?)
                         make_shared<utils::UncertaintyModels::MCSmearingAdlarson>(),
                         utils::UncertaintyModels::Interpolated::Mode_t::MCSmear
                         )
                     )
               : nullptr // no MCSmear
                 ),
    Sig(HistogramFactory("Sig",HistFac), fitparams, FlagWolfgang),
    Ref(HistogramFactory("Ref",HistFac), fitparams, FlagWolfgang)
{
    if(mc_smear)
        LOG(INFO) << "Additional MC Smearing enabled";
    if(fitparams.Fit_Z_vertex) {
        LOG(INFO) << "Fit Z vertex enabled with sigma=" << fitparams.Z_vertex_sigma;
    }

    promptrandom.AddPromptRange({ -7,  7}); // slight offset due to CBAvgTime reference
    promptrandom.AddRandomRange({-65,-10});  // just ensure to be way off prompt peak
    promptrandom.AddRandomRange({ 10, 65});

    h_Cuts = HistFac.makeTH1D("Cuts", "", "#", BinSettings(15),"h_Cuts");
    h_MissedBkg = HistFac.makeTH1D("Missed Background", "", "#", BinSettings(25),"h_MissedBkg");

    h_LostPhotons_sig = HistFac.makeTH1D("LostPhotons Sig", "#theta", "#", BinSettings(200,0,180),"h_LostPhotons_sig");
    h_LostPhotons_ref = HistFac.makeTH1D("LostPhotons Ref", "#theta", "#", BinSettings(200,0,180),"h_LostPhotons_ref");

    if(!FlagWolfgang) {
        t.CreateBranches(Sig.treeCommon);
        t.CreateBranches(Ref.treeCommon);
    }
    t.Tree = nullptr; // prevent accidental misuse...
}

void EtapOmegaG::ProcessEvent(const TEvent& event, manager_t&)
{
    // we start with some general candidate handling,
    // later we split into ref/sig analysis according to
    // number of photons

    const bool have_MCTrue = !event.MCTrue().ID.IsInvalid();

    const TEventData& data = event.Reconstructed();

    const bool is_MC = data.ID.isSet(TID::Flags_t::MC);

    h_Cuts->Fill("Seen",1.0);

    auto& particletree = event.MCTrue().ParticleTree;

    // Count EtaPrimes in MC sample
    h_Cuts->Fill("MCTrue #eta'", 0); // ensure the bin is there...
    if(particletree) {
        // note: this might also match to g p -> eta' eta' p,
        // but this is kinematically forbidden
        if(utils::ParticleTools::FindParticle(ParticleTypeDatabase::EtaPrime, particletree, 1)) {
            h_Cuts->Fill("MCTrue #eta'", 1);
        }
    }

    // do some MCTrue identification (if available)
    t.MCTrue = 0; // indicate data by default
    t.TrueZVertex = event.MCTrue().Target.Vertex.z; // NaN in case of data

    params_t p;
    if(particletree) {
        p.ParticleTree = particletree;

        // 1=Signal, 2=Reference, 9=MissedBkg, >=10 found in ptreeBackgrounds
        if(particletree->IsEqual(ptreeSignal, utils::ParticleTools::MatchByParticleName)) {
            t.MCTrue = 1;
            p.IsSignalTree = true;
        }
        else if(particletree->IsEqual(ptreeReference, utils::ParticleTools::MatchByParticleName)) {
            t.MCTrue = 2;
        }
        else {
            t.MCTrue = 10;
            bool found = false;
            for(const auto& ptreeBkg : ptreeBackgrounds) {
                if(particletree->IsEqual(ptreeBkg.Tree, utils::ParticleTools::MatchByParticleName)) {
                    found = true;
                    break;
                }
                t.MCTrue++;
            }
            if(!found) {
                t.MCTrue = 9;
                const auto& decaystr = utils::ParticleTools::GetDecayString(particletree);
                h_MissedBkg->Fill(decaystr.c_str(), 1.0);
            }
        }
    }
    else if(have_MCTrue) {
        // in rare cases, the particletree is not available, although we're running on MCTrue
        // mark this as other MC background
        t.MCTrue = 9;
    }

    // do some additional counting if true signal/ref event
    if(t.MCTrue == 1 || t.MCTrue == 2) {
        auto h_cut = t.MCTrue == 1 ? Sig.h_Cuts : Ref.h_Cuts;
        auto h_lost = t.MCTrue == 1 ? h_LostPhotons_sig : h_LostPhotons_ref;
        h_cut->Fill("MCTrue seen", 1.0);
        bool photons_accepted = true;
        for(const TParticlePtr& p : event.MCTrue().Particles.Get(ParticleTypeDatabase::Photon)) {
            if(geometry.DetectorFromAngles(*p) == Detector_t::Any_t::None) {
                h_lost->Fill(std_ext::radian_to_degree(p->Theta()));
                photons_accepted = false;
            }
        }
        if(photons_accepted) {
            h_cut->Fill("MCTrue Photon ok", 1.0);
        }
        auto proton = event.MCTrue().Particles.Get(ParticleTypeDatabase::Photon).front();
        if(geometry.DetectorFromAngles(*proton) != Detector_t::Any_t::None)
            h_cut->Fill("MCTrue Proton ok", 1.0);
    }

    // start now with some cuts

    // very simple trigger simulation for MC
    /// \todo Investigate trigger behaviour with pi0pi0 sample?
    if(is_MC) {
        if(data.Trigger.CBEnergySum<=550)
            return;
        h_Cuts->Fill("MC CBEnergySum>550",1.0);
    }
    t.CBSumE = data.Trigger.CBEnergySum;

    t.CBAvgTime = data.Trigger.CBTiming;
    if(!isfinite(t.CBAvgTime))
        return;
    h_Cuts->Fill("CBAvgTime ok",1.0);

    if(data.Candidates.size()<3)
        return;
    h_Cuts->Fill("nCands>=3", 1.0);

    // gather candidates sorted by energy
    TCandidatePtrList candidates;
    bool haveTAPS = false;
    for(const auto& cand : data.Candidates.get_iter()) {
        if(cand->Detector & Detector_t::Type_t::TAPS) {
            haveTAPS = true;
        }
        candidates.emplace_back(cand);
    }
    if(!haveTAPS)
        return;
    h_Cuts->Fill("1 in TAPS",1.0);

    std::sort(candidates.begin(), candidates.end(),
              [] (const TCandidatePtr& a, const TCandidatePtr& b) {
        return a->CaloEnergy > b->CaloEnergy;
    });

    // prepare all proton/photons particle combinations
    {
        TParticleList all_photons;
        TParticleList all_protons;

        for(const auto& cand_proton :  candidates) {
            all_protons.emplace_back(make_shared<TParticle>(ParticleTypeDatabase::Proton, cand_proton));
            all_photons.emplace_back(make_shared<TParticle>(ParticleTypeDatabase::Photon, cand_proton));
        }

        // additionally smear the particles in MC
        if(is_MC && mc_smear) {
            auto smear_particles = [this] (TParticleList& particles) {
                for(auto& p : particles)
                    p = mc_smear->Smear(p);
            };
            smear_particles(all_photons);
            smear_particles(all_protons);
        }

        for(const auto& proton : all_protons) {
            p.Particles.emplace_back(proton);
            auto& photons = p.Particles.back().Photons;
            for(const auto& photon : all_photons) {
                if(proton->Candidate == photon->Candidate)
                    continue;
                photons.emplace_back(photon);
            }
        }
    }

    // sum up the PID energy
    // (might be different to matched CB/PID Veto energy)
    t.PIDSumE = 0;
    for(const TCluster& cl : data.Clusters) {
        if(cl.DetectorType == Detector_t::Type_t::PID) {
            t.PIDSumE += cl.Energy;
        }
    }


    for(const TTaggerHit& taggerhit : data.TaggerHits) {
        promptrandom.SetTaggerHit(taggerhit.Time);
        if(promptrandom.State() == PromptRandom::Case::Outside)
            continue;

        t.TaggW =  promptrandom.FillWeight();
        t.TaggE =  taggerhit.PhotonEnergy;
        t.TaggT =  taggerhit.Time;
        t.TaggCh = taggerhit.Channel;

        p.TaggerHit = taggerhit;

        if(FlagWolfgang)
            Sig.Pi0.t_w.CopyFrom(t);

        Sig.Process(p);

        if(!FlagWolfgang)
            Ref.Process(p);
    }

}

bool EtapOmegaG::params_t::Filter(
        unsigned n, TH1D* h_Cuts,
        double maxDiscardedEk,
        interval<double> missingMassCut,
        interval<double> photonSumCut
        )
{
    h_Cuts->Fill("Seen", 1.0);
    // assume the number of photons is constant for each proton/photon combination
    if(Particles.empty() || Particles.front().Photons.size()<n)
        return false;

    {
        auto it = Particles.begin();
        while(it != Particles.end()) {

            h_Cuts->Fill("Seen protons", 1.0);

            unsigned i=0;
            for(const auto& photon : it->Photons) {
                if(i<n) {
                    it->PhotonSum += *photon;
                }
                else {
                    it->DiscardedEk += photon->Ek();
                }
                ++i;
            }

            if(it->DiscardedEk>maxDiscardedEk) {
                it = Particles.erase(it);
                continue;
            }
            h_Cuts->Fill("DiscEk ok", 1.0);

            const LorentzVec beam_target = TaggerHit.GetPhotonBeam() + LorentzVec({0, 0, 0}, ParticleTypeDatabase::Proton.Mass());
            it->MissingMass = (beam_target - it->PhotonSum).M();

            if(!missingMassCut.Contains(it->MissingMass)) {
                it = Particles.erase(it);
                continue;
            }
            h_Cuts->Fill("MM ok", 1.0);

            if(!photonSumCut.Contains(it->PhotonSum.M())) {
                it = Particles.erase(it);
                continue;
            }
            h_Cuts->Fill("IM ok", 1.0);

            it->Photons.resize(n);
            ++it;
        }
    }

    return !Particles.empty();
}

void EtapOmegaG::ProtonPhotonTree_t::Fill(const EtapOmegaG::params_t& params, const EtapOmegaG::particle_t& p, double fitted_proton_E)
{
    PhotonsEk = 0;
    nPhotonsCB = 0;
    nPhotonsTAPS = 0;
    CBSumVetoE = 0;
    PhotonThetas().clear();
    for(const auto& photon : p.Photons) {
        const auto& cand = photon->Candidate;
        PhotonsEk += cand->CaloEnergy;
        if(cand->Detector & Detector_t::Type_t::CB) {
            nPhotonsCB++;
            CBSumVetoE += cand->VetoEnergy;
        }
        if(cand->Detector & Detector_t::Type_t::TAPS)
            nPhotonsTAPS++;
        PhotonThetas().emplace_back(std_ext::radian_to_degree(cand->Theta));
    }
    assert(PhotonThetas().size() == p.Photons.size());

    DiscardedEk = p.DiscardedEk;
    PhotonSum = p.PhotonSum.M();
    MissingMass = p.MissingMass;
    ProtonCopl = std_ext::radian_to_degree(vec2::Phi_mpi_pi(p.Proton->Phi() - p.PhotonSum.Phi() - M_PI ));

    ProtonTime = p.Proton->Candidate->Time;
    ProtonE = p.Proton->Ek();
    ProtonTheta = std_ext::radian_to_degree(p.Proton->Theta());
    ProtonVetoE = p.Proton->Candidate->VetoEnergy;
    ProtonShortE = p.Proton->Candidate->FindCaloCluster()->ShortEnergy;
    auto true_proton = utils::ParticleTools::FindParticle(ParticleTypeDatabase::Proton, params.ParticleTree);
    if(true_proton)
        ProtonTrueAngle = std_ext::radian_to_degree(p.Proton->Angle(*true_proton));
    else
        ProtonTrueAngle = std_ext::NaN;

    FittedProtonE = fitted_proton_E;
}

EtapOmegaG::Sig_t::Sig_t(const HistogramFactory& HistFac, fitparams_t params, bool flagWolfgang) :
    FlagWolfgang(flagWolfgang),
    h_Cuts(HistFac.makeTH1D("Cuts", "", "#", BinSettings(15),"h_Cuts")),
    treeCommon(FlagWolfgang ? nullptr : HistFac.makeTTree("Common")),
    Pi0(params, FlagWolfgang),
    OmegaPi0(params),
    kinfitter("kinfitter_sig",4,
              params.Fit_uncertainty_model, params.Fit_Z_vertex,
              EtapOmegaG::MakeFitSettings(10)
              ),
    treefitter_Pi0Pi0("treefit_Pi0Pi0",
                      ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::TwoPi0_4g),
                      params.Fit_uncertainty_model, params.Fit_Z_vertex, {},
                      MakeFitSettings(10)
                      ),
    treefitter_Pi0Eta("treefit_Pi0Eta",
                      ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::Pi0Eta_4g),
                      params.Fit_uncertainty_model, params.Fit_Z_vertex, {},
                      MakeFitSettings(10)
                      )
{
    if(!flagWolfgang) {
        t.CreateBranches(HistFac.makeTTree("Shared"));
        OmegaPi0.t.CreateBranches(HistFac.makeTTree("OmegaPi0"));
        Pi0.t.CreateBranches(HistFac.makeTTree("Pi0"));
    }
    else {
        Pi0.t_w.CreateBranches(HistFac.makeTTree("t"));
    }
    if(params.Fit_Z_vertex) {
        kinfitter.SetZVertexSigma(params.Z_vertex_sigma);
        treefitter_Pi0Pi0.SetZVertexSigma(params.Z_vertex_sigma);
        treefitter_Pi0Eta.SetZVertexSigma(params.Z_vertex_sigma);
    }

    {
        auto pi0s = treefitter_Pi0Pi0.GetTreeNodes(ParticleTypeDatabase::Pi0);
        treefitter_Pi0Pi0.SetIterationFilter([pi0s] () {
            auto lvsum1 = pi0s.front()->Get().LVSum;
            auto lvsum2 = pi0s.back()->Get().LVSum;

            const auto& pi0_cut = ParticleTypeDatabase::Pi0.GetWindow(80);

            return pi0_cut.Contains(lvsum1.M()) && pi0_cut.Contains(lvsum2.M());
        });
    }

    {
        auto pi0 = treefitter_Pi0Eta.GetTreeNode(ParticleTypeDatabase::Pi0);
        auto eta = treefitter_Pi0Eta.GetTreeNode(ParticleTypeDatabase::Eta);

        treefitter_Pi0Eta.SetIterationFilter([pi0,eta] () {
            const auto& pi0_lvsum = pi0->Get().LVSum;
            const auto& eta_lvsum = eta->Get().LVSum;

            const auto& pi0_cut = ParticleTypeDatabase::Pi0.GetWindow(80);
            const auto& eta_cut = ParticleTypeDatabase::Eta.GetWindow(120);

            return pi0_cut.Contains(pi0_lvsum.M()) && eta_cut.Contains(eta_lvsum.M());
        });
    }

}

void EtapOmegaG::Sig_t::Process(params_t params)
{
    if(!params.Filter(4, h_Cuts,
                      70.0, ParticleTypeDatabase::Proton.GetWindow(350), {550, std_ext::inf}))
        return;

    t.KinFitProb = std_ext::NaN;

    for(auto& p : params.Particles) {

        kinfitter.SetEgammaBeam(params.TaggerHit.PhotonEnergy);
        kinfitter.SetProton(p.Proton);
        kinfitter.SetPhotons(p.Photons);

        auto result = kinfitter.DoFit();

        if(result.Status != APLCON::Result_Status_t::Success)
            continue;

        if(!std_ext::copy_if_greater(t.KinFitProb, result.Probability))
            continue;

        t.KinFitProb = result.Probability;
        t.KinFitIterations = result.NIterations;
        t.KinFitZVertex = kinfitter.GetFittedZVertex();
    }

    if(!(t.KinFitProb > 0.005))
        return;
    h_Cuts->Fill("KinFit ok", 1.0);

    DoAntiPi0Eta(params);

    if(t.AntiPi0FitProb > 0.05)
        return;
    if(t.AntiEtaFitProb > 0.05)
        return;
    h_Cuts->Fill("Anti ok", 1.0);

    Pi0.Process(params);
    OmegaPi0.Process(params);

    if(!isfinite(Pi0.t.TreeFitProb) && !isfinite(OmegaPi0.t.TreeFitProb))
        return;

    h_Cuts->Fill("Sig ok", 1.0);

    if(isfinite(Pi0.t.TreeFitProb) && isfinite(OmegaPi0.t.TreeFitProb))
        h_Cuts->Fill("Both ok", 1.0);

    h_Cuts->Fill("Pi0 ok", isfinite(Pi0.t.TreeFitProb));
    h_Cuts->Fill("OmegaPi0 ok", isfinite(OmegaPi0.t.TreeFitProb));

    // fill them all to keep them in sync
    if(!FlagWolfgang) {
        treeCommon->Fill();
        t.Tree->Fill();
        Pi0.t.Tree->Fill();
        OmegaPi0.t.Tree->Fill();
    }
    else {
        Pi0.t_w.CopyFrom(t);
        Pi0.t_w.CopyFrom(Pi0.t);
        Pi0.t_w.Tree->Fill();
    }
}

void EtapOmegaG::Sig_t::DoAntiPi0Eta(const params_t& params)
{
    t.AntiPi0FitProb = std_ext::NaN;
    t.AntiEtaFitProb = std_ext::NaN;

    for(const auto& p : params.Particles) {

        APLCON::Result_t r;

        treefitter_Pi0Pi0.SetEgammaBeam(params.TaggerHit.PhotonEnergy);
        treefitter_Pi0Pi0.SetProton(p.Proton);
        treefitter_Pi0Pi0.SetPhotons(p.Photons);
        while(treefitter_Pi0Pi0.NextFit(r)) {
            if(r.Status != APLCON::Result_Status_t::Success)
                continue;
            if(!std_ext::copy_if_greater(t.AntiPi0FitProb, r.Probability))
                continue;
            // found fit with better prob
            t.AntiPi0FitIterations = r.NIterations;

            const auto& fitter = treefitter_Pi0Pi0;
            t.AntiPi0FitZVertex = fitter.GetFittedZVertex();
        }

        treefitter_Pi0Eta.SetEgammaBeam(params.TaggerHit.PhotonEnergy);
        treefitter_Pi0Eta.SetProton(p.Proton);
        treefitter_Pi0Eta.SetPhotons(p.Photons);
        while(treefitter_Pi0Eta.NextFit(r)) {
            if(r.Status != APLCON::Result_Status_t::Success)
                continue;
            if(!std_ext::copy_if_greater(t.AntiEtaFitProb, r.Probability))
                continue;
            // found fit with better probability
            t.AntiEtaFitIterations = r.NIterations;

            const auto& fitter = treefitter_Pi0Eta;
            t.AntiEtaFitZVertex = fitter.GetFittedZVertex();
        }
    }
}

EtapOmegaG::Sig_t::Fit_t::Fit_t(utils::TreeFitter fitter) :
    treefitter(move(fitter)),
    fitted_Pi0(treefitter.GetTreeNode(ParticleTypeDatabase::Pi0)),
    fitted_Omega(treefitter.GetTreeNode(ParticleTypeDatabase::Omega)),
    fitted_EtaPrime(treefitter.GetTreeNode(ParticleTypeDatabase::EtaPrime))
{

    // search dependent gammas and remember the tree nodes in the fitter

    auto find_photons = [] (utils::TreeFitter::tree_t fitted) {
        std::vector<utils::TreeFitter::tree_t> photons;
        for(const auto& d : fitted->Daughters())
            if(d->Get().TypeTree->Get() == ParticleTypeDatabase::Photon)
                photons.emplace_back(d);
        return photons;
    };

    fitted_g1_Pi0 = find_photons(fitted_Pi0).at(0);
    fitted_g2_Pi0 = find_photons(fitted_Pi0).at(1);

    fitted_g_Omega = find_photons(fitted_Omega).at(0);

    fitted_g_EtaPrime = find_photons(fitted_EtaPrime).at(0);

    {
        treefitter.SetIterationFilter([this] () {
            const auto& pi0 = fitted_Pi0->Get().LVSum;
            double invchi2 = 1.0/std_ext::sqr(ParticleTypeDatabase::Pi0.Mass() - pi0.M());
            if(fitted_Omega) {
                const auto& omega = fitted_Omega->Get().LVSum;
                invchi2 += 1.0/std_ext::sqr(ParticleTypeDatabase::Omega.Mass() - omega.M());
            }
            return invchi2;
        },
        4);
    }
}

utils::TreeFitter EtapOmegaG::Sig_t::Fit_t::Make(const ParticleTypeDatabase::Type& subtree, fitparams_t params)
{
    auto setupnodes = [&subtree] (const ParticleTypeTree& t) {
        utils::TreeFitter::nodesetup_t nodesetup;
        // always exlude the EtaPrime
        if(t->Get() == ParticleTypeDatabase::EtaPrime)
            nodesetup.Excluded = true;
        // subtree decides if the Omega is excluded or not
        if(subtree == ParticleTypeDatabase::Pi0 &&
           t->Get() == ParticleTypeDatabase::Omega)
            nodesetup.Excluded = true;
        return nodesetup;
    };

    utils::TreeFitter treefitter{
        "sig_treefitter_"+subtree.Name(),
                EtapOmegaG::ptreeSignal,
                params.Fit_uncertainty_model,
                params.Fit_Z_vertex,
                setupnodes,
                MakeFitSettings(15)
    };
    if(params.Fit_Z_vertex)
        treefitter.SetZVertexSigma(params.Z_vertex_sigma);
    return treefitter;
}

void fill_gNonPi0(
        EtapOmegaG::Sig_t::Fit_t::BaseTree_t& t,
        const TCandidatePtr& cand1, const TCandidatePtr& cand2)
{
    t.gNonPi0_Theta().front() = cand1->Theta;
    t.gNonPi0_Theta().back()  = cand2->Theta;

    t.gNonPi0_CaloE().front() = cand1->CaloEnergy;
    t.gNonPi0_CaloE().back() = cand2->CaloEnergy;
}

void fill_PhotonCombs(EtapOmegaG::Sig_t::Fit_t::BaseTree_t& t, const TParticleList& photons)
{
    //  ggg combinatorics
    auto it_ggg = t.ggg().begin();
    for( auto comb = utils::makeCombination(photons,3); !comb.Done(); ++comb ) {
        *it_ggg = (*comb.at(0) + *comb.at(1) + *comb.at(2)).M();
        ++it_ggg;
    }

    // gg/gg "Goldhaber" combinatorics
    const auto goldhaber_comb = vector<vector<unsigned>>({{0,1,2,3},{0,2,1,3},{0,3,1,2}});
    auto it_gg_gg1 = t.gg_gg1().begin();
    auto it_gg_gg2 = t.gg_gg2().begin();
    for(auto i : goldhaber_comb) {
        const auto& p = photons;
        *it_gg_gg1 = (*p[i[0]] + *p[i[1]]).M();
        *it_gg_gg2 = (*p[i[2]] + *p[i[3]]).M();
        ++it_gg_gg1;
        ++it_gg_gg2;
    }
}


EtapOmegaG::Sig_t::Pi0_t::Pi0_t(fitparams_t params, bool flagWolfgang) :
    Fit_t(Fit_t::Make(ParticleTypeDatabase::Pi0, params)),
    FlagWolfgang(flagWolfgang)
{

}

void EtapOmegaG::Sig_t::Pi0_t::Process(const params_t& params)
{
    t.TreeFitProb = std_ext::NaN;
    t.MCTrueMatch = 0;

    // for MCtrue identification
    TParticlePtr g1_Pi0_best;
    TParticlePtr g2_Pi0_best;
    TParticleList photons_best;

    for(const auto& p : params.Particles) {

        // do treefit
        treefitter.SetEgammaBeam(params.TaggerHit.PhotonEnergy);
        treefitter.SetProton(p.Proton);
        treefitter.SetPhotons(p.Photons);

        APLCON::Result_t r;

        while(treefitter.NextFit(r)) {
            if(r.Status != APLCON::Result_Status_t::Success)
                continue;
            if(!std_ext::copy_if_greater(t.TreeFitProb, r.Probability))
                continue;
            // found fit with better prob
            t.TreeFitIterations = r.NIterations;
            t.TreeFitZVertex = treefitter.GetFittedZVertex();

            // for MCTrue matching
            g1_Pi0_best = fitted_g1_Pi0->Get().Leave->Particle;
            g2_Pi0_best = fitted_g2_Pi0->Get().Leave->Particle;
            photons_best = p.Photons;

            // IM fitted expected to be delta peaks since they were fitted...
            const LorentzVec& Pi0 = fitted_Pi0->Get().LVSum;
            t.IM_Pi0 = Pi0.M();

            t.IM_Pi0gg = fitted_EtaPrime->Get().LVSum.M();

            // there are two photon combinations possible for the omega
            // MC shows that it's the one with the higher IM_3g = IM_Pi0g
            auto leave1 = fitted_g_Omega->Get().Leave;
            auto leave2 = fitted_g_EtaPrime->Get().Leave;
            LorentzVec g1 = *leave1->AsFitted();
            LorentzVec g2 = *leave2->AsFitted();

            // invariant under swap
            t.IM_gg = (g1 + g2).M();
            const LorentzVec EtaPrime = g1 + g2 + Pi0;

            t.IM_Pi0g().front() = (Pi0 + g1).M();
            t.IM_Pi0g().back()  = (Pi0 + g2).M();
            if(t.IM_Pi0g().front() > t.IM_Pi0g().back()) {
                std::swap(t.IM_Pi0g().front(), t.IM_Pi0g().back());
                std::swap(leave1, leave2);
                std::swap(g1, g2);
            }

            // g1/leave1 is now the EtaPrime, g2/leave2 is now the Omega bachelor photon

            t.Bachelor_E().front() = Boost(g1, -EtaPrime.BoostVector()).E;
            t.Bachelor_E().back() =  Boost(g2, -EtaPrime.BoostVector()).E;

            fill_gNonPi0(t, leave1->Particle->Candidate, leave2->Particle->Candidate);
            fill_PhotonCombs(t, p.Photons);
            t.Fill(params, p, treefitter.GetFittedProton()->Ek());

            if(FlagWolfgang) {
                t_w.Proton = *treefitter.GetFittedProton();
                t_w.Photon1 = *fitted_g1_Pi0->Get().Leave->AsFitted();
                t_w.Photon2 = *fitted_g2_Pi0->Get().Leave->AsFitted();
                t_w.Photon3 = g1;
                t_w.Photon4 = g2;
            }
        }

    }

    // there was at least one successful fit
    if(isfinite(t.TreeFitProb)) {

        // check MC matching
        if(params.IsSignalTree) {
            auto& ptree_sig = params.ParticleTree;
            auto true_photons = utils::ParticleTools::FindParticles(ParticleTypeDatabase::Photon, ptree_sig);
            assert(true_photons.size() == 4);
            auto match_bycandidate = [] (const TParticlePtr& mctrue, const TParticlePtr& recon) {
                return mctrue->Angle(*recon->Candidate); // TCandidate converts into vec3
            };
            auto matched = utils::match1to1(true_photons, photons_best,
                                            match_bycandidate,IntervalD(0.0, std_ext::degree_to_radian(15.0)));
            if(matched.size() == 4) {
                // find the two photons of the pi0
                TParticleList pi0_photons;
                ptree_sig->Map_nodes([&pi0_photons] (const TParticleTree_t& t) {
                    const auto& parent = t->GetParent();
                    if(!parent)
                        return;
                    if(parent->Get()->Type() == ParticleTypeDatabase::Pi0) {
                        pi0_photons.push_back(t->Get());
                    }
                });
                TParticleList g_pi0_matched{
                    utils::FindMatched(matched, pi0_photons.front()),
                            utils::FindMatched(matched, pi0_photons.back())
                };

                if(std_ext::contains(g_pi0_matched, g1_Pi0_best))
                    t.MCTrueMatch += 1;
                if(std_ext::contains(g_pi0_matched, g2_Pi0_best))
                    t.MCTrueMatch += 2;
            }
        }

    }

}


EtapOmegaG::Sig_t::OmegaPi0_t::OmegaPi0_t(fitparams_t params) :
    Fit_t(Fit_t::Make(ParticleTypeDatabase::Omega, params))
{

}

void EtapOmegaG::Sig_t::OmegaPi0_t::Process(const params_t& params)
{
    t.TreeFitProb = std_ext::NaN;
    t.MCTrueMatch = 0;

    // for MCTrue identification
    TParticlePtr g_Omega_best;
    TParticlePtr g_EtaPrime_best;
    TParticleList photons_best;

    // the EtaPrime bachelor photon is most important to us...
    TParticlePtr g_EtaPrime_fitted;
    LorentzVec EtaPrime_fitted;

    for(const auto& p : params.Particles) {

        // do treefit
        treefitter.SetEgammaBeam(params.TaggerHit.PhotonEnergy);
        treefitter.SetProton(p.Proton);
        treefitter.SetPhotons(p.Photons);

        APLCON::Result_t r;

        while(treefitter.NextFit(r)) {
            if(r.Status != APLCON::Result_Status_t::Success)
                continue;
            if(!std_ext::copy_if_greater(t.TreeFitProb, r.Probability))
                continue;
            // found fit with better prob
            t.TreeFitIterations = r.NIterations;
            t.TreeFitZVertex = treefitter.GetFittedZVertex();

            // IM fitted expected to be delta peaks since they were fitted...
            EtaPrime_fitted = fitted_EtaPrime->Get().LVSum;
            t.IM_Pi0gg = EtaPrime_fitted.M();
            t.IM_Pi0g = fitted_Omega->Get().LVSum.M();
            t.IM_Pi0 = fitted_Pi0->Get().LVSum.M();

            // remember for matching
            g_EtaPrime_best = fitted_g_EtaPrime->Get().Leave->Particle; // unfitted for matching
            g_Omega_best    = fitted_g_Omega->Get().Leave->Particle; // unfitted for matching
            photons_best    = p.Photons;

            // have a look at the EtaPrime bachelor photon
            // the element NOT in the combination is the Bachelor photon
            g_EtaPrime_fitted = fitted_g_EtaPrime->Get().Leave->AsFitted();

            t.IM_gg = ( *fitted_g_EtaPrime->Get().Leave->AsFitted()
                        + *fitted_g_Omega->Get().Leave->AsFitted()).M();

            fill_gNonPi0(t,
                         fitted_g_EtaPrime->Get().Leave->Particle->Candidate,
                         fitted_g_Omega->Get().Leave->Particle->Candidate);
            fill_PhotonCombs(t, p.Photons);
            t.Fill(params, p, treefitter.GetFittedProton()->Ek());
        }

    }

    // there was at least one successful fit
    if(isfinite(t.TreeFitProb)) {

        t.Bachelor_E = Boost(*g_EtaPrime_fitted, -EtaPrime_fitted.BoostVector()).E;

        // check MC matching
        if(params.IsSignalTree) {
            auto& ptree_sig = params.ParticleTree;
            auto true_photons = utils::ParticleTools::FindParticles(ParticleTypeDatabase::Photon, ptree_sig);
            assert(true_photons.size() == 4);
            auto match_bycandidate = [] (const TParticlePtr& mctrue, const TParticlePtr& recon) {
                return mctrue->Angle(*recon->Candidate); // TCandidate converts into vec3
            };
            auto matched = utils::match1to1(true_photons, photons_best,
                                            match_bycandidate,
                                            IntervalD(0.0, std_ext::degree_to_radian(15.0)));
            if(matched.size() == 4) {
                // do that tedious photon determination (rewriting the matcher somehow would be nice....)
                auto select_daughter = [] (TParticleTree_t tree, const ParticleTypeDatabase::Type& type) {
                    auto d = tree->Daughters().front()->Get()->Type() == type ?
                                 tree->Daughters().front() : tree->Daughters().back();
                    assert(d->Get()->Type() == type);
                    return d;
                };

                auto etap = select_daughter(ptree_sig, ParticleTypeDatabase::EtaPrime);
                auto g_EtaPrime = select_daughter(etap, ParticleTypeDatabase::Photon);
                auto omega = select_daughter(etap, ParticleTypeDatabase::Omega);
                auto g_Omega = select_daughter(omega, ParticleTypeDatabase::Photon);

                auto g_EtaPrime_matched = utils::FindMatched(matched, g_EtaPrime->Get());
                auto g_Omega_matched = utils::FindMatched(matched, g_Omega->Get());
                if(g_EtaPrime_matched == g_EtaPrime_best)
                    t.MCTrueMatch += 1;
                if(g_Omega_matched == g_Omega_best)
                    t.MCTrueMatch += 2;
            }
        }
    }
}

EtapOmegaG::Ref_t::Ref_t(const HistogramFactory& HistFac, EtapOmegaG::fitparams_t params, bool flagWolfgang) :
    h_Cuts(HistFac.makeTH1D("Cuts", "", "#", BinSettings(15),"h_Cuts")),
    treeCommon(flagWolfgang ? nullptr :HistFac.makeTTree("Common")),
    kinfitter("kinfitter_ref",2,
              params.Fit_uncertainty_model, params.Fit_Z_vertex,
              EtapOmegaG::MakeFitSettings(15)
              )
{
    if(!flagWolfgang)
        t.CreateBranches(HistFac.makeTTree("Ref"));
    if(params.Fit_Z_vertex)
        kinfitter.SetZVertexSigma(params.Z_vertex_sigma);
}

void EtapOmegaG::Ref_t::Process(params_t params)
{
    if(!params.Filter(2, h_Cuts,
                      70.0, ParticleTypeDatabase::Proton.GetWindow(350), {600, std_ext::inf})
       )
        return;

    t.KinFitProb = std_ext::NaN;
    for(const auto& p : params.Particles) {

        kinfitter.SetEgammaBeam(params.TaggerHit.PhotonEnergy);
        kinfitter.SetProton(p.Proton);
        kinfitter.SetPhotons(p.Photons);

        auto result = kinfitter.DoFit();

        if(result.Status != APLCON::Result_Status_t::Success)
            continue;

        if(!std_ext::copy_if_greater(t.KinFitProb, result.Probability))
            continue;

        t.KinFitProb = result.Probability;
        t.KinFitIterations = result.NIterations;
        t.KinFitZVertex = kinfitter.GetFittedZVertex();

        t.Fill(params, p, kinfitter.GetFittedProton()->Ek());

        const auto& fittedPhotons = kinfitter.GetFittedPhotons();
        t.IM_2g = (*fittedPhotons.front() + *fittedPhotons.back()).M();
    }

    if(t.KinFitProb>0.005) {
        h_Cuts->Fill("Fill", 1.0);
        treeCommon->Fill();
        t.Tree->Fill();
    }

}

void EtapOmegaG::ShowResult()
{
    if(FlagWolfgang)
        return;

    canvas("Overview") << h_Cuts << h_MissedBkg
                       << Sig.h_Cuts << Ref.h_Cuts
                       << h_LostPhotons_sig << h_LostPhotons_ref
                       << endc;


    canvas("Reference")
            << TTree_drawable(Ref.t.Tree, "IM_2g >> (200,650,1050)")
            << endc;

    Sig.Pi0.t.Tree->AddFriend(Sig.t.Tree);
    Sig.OmegaPi0.t.Tree->AddFriend(Sig.t.Tree);
    Sig.Pi0.t.Tree->AddFriend(Sig.treeCommon);
    Sig.OmegaPi0.t.Tree->AddFriend(Sig.treeCommon);

    canvas("Signal")
            << TTree_drawable(Sig.OmegaPi0.t.Tree, "Bachelor_E >> (100,50,250)","(TreeFitProb>0.01)*TaggW")
            << TTree_drawable(Sig.Pi0.t.Tree, "Bachelor_E[0] >> (100,50,250)","(TreeFitProb>0.01)*TaggW")
            << TTree_drawable(Sig.OmegaPi0.t.Tree, "IM_Pi0gg >> (150,750,1100)","(TreeFitProb>0.01)*TaggW")
            << TTree_drawable(Sig.Pi0.t.Tree, "IM_Pi0gg >> (150,750,1100)","(TreeFitProb>0.01)*TaggW")
            << TTree_drawable(Sig.OmegaPi0.t.Tree, "MCTrueMatch")
            << TTree_drawable(Sig.Pi0.t.Tree, "MCTrueMatch")
            << endc;
}


const std::vector<EtapOmegaG::Background_t> EtapOmegaG::ptreeBackgrounds = {
    {"1Pi0", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::Pi0_2g)},
    {"2Pi0", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::TwoPi0_4g)},
    {"Pi0Eta", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::Pi0Eta_4g)},
    {"3Pi0", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::ThreePi0_6g)},
    {"OmegaPi0g", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::Omega_gPi0_3g)},
    {"OmegaPi0PiPPiM", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::Omega_Pi0PiPPiM_2g)},
    {"EtaP2Pi0Eta", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::EtaPrime_2Pi0Eta_6g)},
    {"2Pi0Dalitz", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::TwoPi0_2ggEpEm)},
    {"3Pi0Dalitz", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::ThreePi0_4ggEpEm)},
    {"1Eta", ParticleTypeTreeDatabase::Get(ParticleTypeTreeDatabase::Channel::Eta_2g)},
};

AUTO_REGISTER_PHYSICS(EtapOmegaG)
