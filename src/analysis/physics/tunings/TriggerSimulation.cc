#include "TriggerSimulation.h"

#include "expconfig/ExpConfig.h"
#include "utils/uncertainties/Interpolated.h"
#include "utils/ProtonPhotonCombs.h"
#include "utils/Combinatorics.h"
#include "plot/CutTree.h"
#include "plot/HistStyle.h"
#include "physics/Plotter.h"

using namespace ant;
using namespace ant::analysis;
using namespace ant::analysis::physics;
using namespace ant::analysis::plot;
using namespace std;

TriggerSimulation::TriggerSimulation(const string& name, OptionsPtr opts) :
    Physics(name, opts),
    promptrandom(ExpConfig::Setup::Get()),
    Clusters_All(HistogramFactory("Clusters_All",HistFac,"Clusters_All")),
    Clusters_Tail(HistogramFactory("Clusters_Tail",HistFac,"Clusters_Tail")),
    fitter(utils::UncertaintyModels::Interpolated::makeAndLoad(), true, // enable z vertex
           // in place generation of settings, too lazy to write static method
           [] () { APLCON::Fit_Settings_t settings; settings.MaxIterations = 10; return settings;}()
           )
{
    fitter.SetZVertexSigma(0); // unmeasured z-vertex

    steps = HistFac.makeTH1D("Steps","","#",BinSettings(10),"steps");

    const AxisSettings axis_CBESum{"CBESum / MeV", {1600, 0, 1600}};
    const AxisSettings axis_CBTiming("CB Timing / ns",{300,-15,10});

    h_CBESum_raw = HistFac.makeTH1D("CBESum raw ",axis_CBESum,"h_CBESum_raw");
    h_CBESum_pr  = HistFac.makeTH1D("CBESum raw p-r sub",axis_CBESum,"h_CBESum_pr");
    h_CBESum_fit  = HistFac.makeTH1D("CBESum fit p-r sub",axis_CBESum,"h_CBESum_fit");

    h_CBTiming       = HistFac.makeTH1D("CB Timing", axis_CBTiming, "h_CBTiming");
    h_CBTiming_CaloE = HistFac.makeTH2D("CB Timing vs. CaloE",axis_CBTiming,{"CaloE / MeV", {200,0,100}},"h_CBTiming_CaloE");

    const BinSettings bins_TaggT(200,-30,30);
    h_TaggT = HistFac.makeTH1D("Tagger Timing",{"t_{Tagger}", bins_TaggT},"h_TaggT");
    h_TaggT_corr = HistFac.makeTH1D("Tagger Timing Corrected",{"t_{Tagger} Corrected", bins_TaggT},"h_TaggT_corr");
    h_TaggT_CBTiming = HistFac.makeTH2D("Tagger Timing vs. CBTiming",{"t_{Tagger}", bins_TaggT},axis_CBTiming,"h_TaggT_CBTiming");

    t.CreateBranches(HistFac.makeTTree("tree"));
}

TriggerSimulation::ClusterPlots_t::ClusterPlots_t(const HistogramFactory& HistFac)
{
    const AxisSettings axis_CaloE("CaloE / MeV",{100,0,20});
    const AxisSettings axis_ClSize("ClusterSize",{10});
    const AxisSettings axis_nCl("nClusters",{10});
    const AxisSettings axis_timing("t / ns",{100,-30,30});

    h_CaloE_ClSize = HistFac.makeTH2D("CaloE vs. ClusterSize", axis_CaloE, axis_ClSize, "h_CaloE_ClSize");
    h_CaloE_nCl = HistFac.makeTH2D("CaloE vs. nClusters", axis_CaloE, axis_nCl, "h_CaloE_nCl");
    h_CaloE_Time = HistFac.makeTH2D("CaloE vs. Time",axis_CaloE, axis_timing,"h_CaloE_Time");
    h_Hits_stat = HistFac.makeTH1D("Hits status","","",BinSettings(4),"h_Hits_stat");
    h_Hits_E_t  = HistFac.makeTH2D("ClHits Energy vs. Time",{"E_{hit} / MeV",{100,0,50}}, axis_timing ,"h_Hits_E_t");
}

void TriggerSimulation::ClusterPlots_t::Fill(const TEventData& recon) const
{
    for(const TCluster& cluster : recon.Clusters) {
        if(cluster.DetectorType == Detector_t::Type_t::CB) {
            h_CaloE_ClSize->Fill(cluster.Energy,cluster.Hits.size());
            h_CaloE_nCl->Fill(cluster.Energy,recon.Clusters.size());
            h_CaloE_Time->Fill(cluster.Energy, cluster.Time);
            for(const auto& hit : cluster.Hits) {
                h_Hits_E_t->Fill(hit.Energy, hit.Time);
                h_Hits_stat->Fill("Seen",1.0);
                if(hit.IsSane())
                    h_Hits_stat->Fill("Sane",1.0);
                if(isfinite(hit.Time))
                    h_Hits_stat->Fill("Time ok",1.0);
                if(isfinite(hit.Time))
                    h_Hits_stat->Fill("Energy ok",1.0);
            }
        }
    }
}

void TriggerSimulation::ClusterPlots_t::Show(canvas &c) const
{
    c << drawoption("colz")
      << h_CaloE_ClSize << h_CaloE_nCl << h_CaloE_Time
      << h_Hits_stat << h_Hits_E_t
      << endr;
}

void TriggerSimulation::ProcessEvent(const TEvent& event, manager_t&)
{

    steps->Fill("Seen",1);

    if(!triggersimu.ProcessEvent(event)) {
        steps->Fill("TriggerSimu failed", 1.0);
        return;
    }

    steps->Fill("Triggered", triggersimu.HasTriggered());

    // as MC may have also some pure TAPS events,
    // zero CBEnergySum can be suppressed,
    // as we can't return when we're not triggered
    // (that's what we want to determine)
    if(triggersimu.GetCBEnergySum()==0)
        return;

    const auto& recon = event.Reconstructed();

    // gather tree stuff already here, before we forget it :)
    t.IsMC = recon.ID.isSet(TID::Flags_t::MC);
    t.Triggered = triggersimu.HasTriggered();
    t.CBEnergySum = triggersimu.GetCBEnergySum();

    h_CBESum_raw->Fill(triggersimu.GetCBEnergySum());
    h_CBTiming->Fill(triggersimu.GetRefTiming());
    for(const TCluster& cluster : recon.Clusters) {
        if(cluster.DetectorType == Detector_t::Type_t::CB) {
            h_CBTiming_CaloE->Fill(triggersimu.GetRefTiming(),cluster.Energy);
        }
    }

    Clusters_All.Fill(recon);
    if(IntervalD(-10,-5).Contains(triggersimu.GetRefTiming())) {
        // investigate the tail
        Clusters_Tail.Fill(recon);
    }

    t.nPhotons = recon.Candidates.size()-1; // is at least 2
    if(t.nPhotons<2)
        return;

    if(t.nPhotons != 2 && t.nPhotons != 4)
        return;

    utils::ProtonPhotonCombs proton_photons(recon.Candidates);

    for(const TTaggerHit& taggerhit : recon.TaggerHits) {

        steps->Fill("Seen taggerhits",1.0);

        h_TaggT->Fill(taggerhit.Time);
        h_TaggT_CBTiming->Fill(taggerhit.Time, triggersimu.GetRefTiming());
        const auto& taggertime = triggersimu.GetCorrectedTaggerTime(taggerhit);
        h_TaggT_corr->Fill(taggertime);

        promptrandom.SetTaggerTime(taggertime);
        if(promptrandom.State() == PromptRandom::Case::Outside)
            continue;

        steps->Fill("Acc taggerhits",1.0);

        h_CBESum_pr->Fill(triggersimu.GetCBEnergySum(), promptrandom.FillWeight());

        t.TaggW = promptrandom.FillWeight();
        t.TaggT = taggertime;
        t.TaggE = taggerhit.PhotonEnergy;
        t.TaggCh = taggerhit.Channel;

        t.nPhotons = recon.Candidates.size()-1; // is at least 2

        // setup a very inclusive filter, just to speed up fitting
        auto filtered_combs = proton_photons()
                              .Observe([this] (const string& cut) { steps->Fill(cut.c_str(), 1.0); }, "F ")
                              .FilterMM(taggerhit, ParticleTypeDatabase::Proton.GetWindow(300).Round());

        if(filtered_combs.empty()) {
            steps->Fill("No combs left",1.0);
            continue;
        }

        // loop over the (filtered) proton combinations
        t.FitProb = std_ext::NaN;
        for(const auto& comb : filtered_combs) {

            const auto& result = fitter.DoFit(taggerhit.PhotonEnergy, comb.Proton, comb.Photons);

            if(result.Status != APLCON::Result_Status_t::Success)
                continue;
            if(!std_ext::copy_if_greater(t.FitProb, result.Probability))
                continue;

            t.ZVertex = fitter.GetFittedZVertex();

            // do combinatorics
            const auto fill_IM_Combs = [] (vector<double>& v, const TParticleList& photons) {
                auto combs = utils::makeCombination(photons, 2);
                v.resize(combs.size());
                for(auto& im : v) {
                    im = (*combs.at(0) + *combs.at(1)).M();
                    combs.next();
                }
            };

            fill_IM_Combs(t.IM_Combs_fitted, fitter.GetFittedPhotons());
            fill_IM_Combs(t.IM_Combs_raw, comb.Photons);
        }

        if(t.FitProb>0.01) {
            steps->Fill("FitProb>0.01",1.0);
            t.Tree->Fill();
            h_CBESum_fit->Fill(t.CBEnergySum, t.TaggW);
        }

    }
}

void TriggerSimulation::ShowResult()
{
    canvas(GetName()) << drawoption("colz")
            << steps
            << h_TaggT << h_TaggT_CBTiming << h_TaggT_corr
            << h_CBTiming
            << h_CBESum_raw << h_CBESum_pr << h_CBESum_fit
            << TTree_drawable(t.Tree, "ZVertex")
            << endc;
    canvas c(GetName()+": CBTiming Tail");
    Clusters_All.Show(c);
    Clusters_Tail.Show(c);
    c << endc;
}

struct Hist_t {
    using Tree_t = physics::TriggerSimulation::Tree_t;

    // using Fill_t = Tree_t could also be used, but
    // having it a more complex struct provides more flexibility
    // for example providing the tagger weight and a handy Fill() method
    struct Fill_t {
        const Tree_t& Tree;
        Fill_t(const Tree_t& tree) : Tree(tree) {}

        double Weight() const {
            return Tree.TaggW;
        }

        template<typename H, typename... Args>
        void Fill(const H& h, Args&&... args) const {
            h->Fill(std::forward<Args>(args)..., Weight());
        }
    };

    TH1D* h_FitProb = nullptr;
    TH1D* h_CBEnergySum = nullptr;
    TH1D* h_IM_2g_fitted = nullptr;
    TH1D* h_IM_2g_raw = nullptr;

    const bool isLeaf = false;


    Hist_t(HistogramFactory HistFac, cuttree::TreeInfo_t treeInfo) :
        isLeaf(treeInfo.nDaughters==0)
    {
        h_FitProb      = HistFac.makeTH1D("KinFit Probability",{"p",{100, 0, 1}},"h_FitProb");
        h_CBEnergySum  = HistFac.makeTH1D("CB Energy Sum",{"E / MeV",{1600, 0, 1600}},"h_CBEnergySum");
        const AxisSettings bins_IM{"IM(2#gamma) / MeV",{1600,0,1600}};
        h_IM_2g_fitted = HistFac.makeTH1D("IM 2g Combs (fitted)",bins_IM,"h_IM_2g_fitted");
        h_IM_2g_raw    = HistFac.makeTH1D("IM 2g Combs (raw after fit)",bins_IM,"h_IM_2g_raw");
    }


    void Fill(const Fill_t& f) const {
        f.Fill(h_FitProb, f.Tree.FitProb);
        f.Fill(h_CBEnergySum, f.Tree.CBEnergySum);
        for(auto& im : f.Tree.IM_Combs_fitted())
            f.Fill(h_IM_2g_fitted, im);
        for(auto& im : f.Tree.IM_Combs_raw())
            f.Fill(h_IM_2g_raw, im);
    }

    std::vector<TH1*> GetHists() const {
        return {h_FitProb, h_CBEnergySum, h_IM_2g_fitted, h_IM_2g_raw};
    }

    // Sig and Ref channel (can) share some cuts...
    static cuttree::Cuts_t<Fill_t> GetCuts() {
        using cuttree::MultiCut_t;
        cuttree::Cuts_t<Fill_t> cuts;
        cuts.emplace_back(MultiCut_t<Fill_t>{
                              {"Triggered", [] (const Fill_t& f) { return f.Tree.Triggered(); } },
                              {"-"}, // no lambda means no cut (always true returned)
                          });
//        cuts.emplace_back(MultiCut_t<Fill_t>{
//                              {"FitProb>0.01", [] (const Fill_t& f) { return f.Tree.FitProb>0.01; } },
//                              {"-", [] (const Fill_t&) { return true; } },
//                          });
        return cuts;
    }
};

struct DataMC_Splitter : cuttree::StackedHists_t<Hist_t> {

    // Hist_t should have that type defined,
    // we borrow it from the underlying Hist_t
    using Fill_t = typename Hist_t::Fill_t;

    DataMC_Splitter(const HistogramFactory& histFac,
                    const cuttree::TreeInfo_t& treeInfo) :
        cuttree::StackedHists_t<Hist_t>(histFac, treeInfo)
    {
        using histstyle::Mod_t;
        this->GetHist(0, "Data", Mod_t::MakeDataPoints(kBlack));
        this->GetHist(1, "MC",   Mod_t::MakeLine(kBlack, 2.0));
    }

    void Fill(const Fill_t& f) {
        const Hist_t& hist = this->GetHist(f.Tree.IsMC);
        hist.Fill(f);
    }
};


struct TriggerSimulation_plot : Plotter {

    Hist_t::Tree_t tree;

    cuttree::Tree_t<DataMC_Splitter> mycuttree;

    TriggerSimulation_plot(const string& name, const WrapTFileInput& input, OptionsPtr opts) :
        Plotter(name, input, opts)
    {
        if(!input.GetObject("TriggerSimulation/tree", tree.Tree))
            throw Exception("Cannot find tree TriggerSimulation/tree");
        tree.LinkBranches();

        mycuttree = cuttree::Make<DataMC_Splitter>(HistFac);
    }

    virtual long long GetNumEntries() const override
    {
        return tree.Tree->GetEntries();
    }

    virtual void ProcessEntry(const long long entry) override
    {
        tree.Tree->GetEntry(entry);
        cuttree::Fill<DataMC_Splitter>(mycuttree, tree);
    }

    virtual void ShowResult() override {
        canvas c(GetName());
        mycuttree->Get().Hist.Draw(c);
        c << endc;
    }

};

AUTO_REGISTER_PHYSICS(TriggerSimulation)
AUTO_REGISTER_PLOTTER(TriggerSimulation_plot)