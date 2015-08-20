#include "CB_TimeWalk.h"

#include "DataManager.h"
#include "gui/CalCanvas.h"
#include "fitfunctions/FitTimewalk.h"

#include "reconstruct/Clustering.h"

#include "expconfig/detectors/CB.h"
#include "base/Logger.h"
#include "base/std_ext.h"

#include "TGraph.h"
#include "TFitResult.h"
#include "TObjArray.h"

#include <tree/TCluster.h>

#include <limits>
#include <cmath>

using namespace ant;
using namespace ant::calibration;
using namespace ant::analysis;
using namespace ant::analysis::data;
using namespace std;

CB_TimeWalk::ThePhysics::ThePhysics(const string& name, const std::shared_ptr<expconfig::detector::CB>& cb) :
    Physics(name),
    cb_detector(cb)
{
    h_timewalk = HistFac.makeTH3D(
                     "CB TimeWalk",
                     "Energy / MeV",
                     "Time / ns",
                     "Channel",
                     BinSettings(400,0,1000),
                     BinSettings(100,-100,100),
                     BinSettings(cb_detector->GetNChannels()),
                     "timewalk"
                     );
}

void CB_TimeWalk::ThePhysics::ProcessEvent(const Event& event)
{
    for(const auto& cand: event.Reconstructed().Candidates()) {
        for(const Cluster& cluster: cand->Clusters) {
            if(cluster.Detector != Detector_t::Type_t::CB)
                continue;
            for(const Cluster::Hit& hit : cluster.Hits) {
                // found the hit of the central element
                // now search for its timing information
                double time = numeric_limits<double>::quiet_NaN();
                double energy = numeric_limits<double>::quiet_NaN();
                for(const Cluster::Hit::Datum& d : hit.Data) {
                    if(d.Type == Channel_t::Type_t::Timing)
                        time = d.Value;
                    if(d.Type == Channel_t::Type_t::Integral)
                        energy = d.Value;
                }
                h_timewalk->Fill(energy, time, hit.Channel);
            }
        }
    }
}

void CB_TimeWalk::ThePhysics::Finish()
{

}

void CB_TimeWalk::ThePhysics::ShowResult()
{
    canvas c(GetName());
    c << drawoption("colz");
    TH2* result = dynamic_cast<TH2*>(h_timewalk->Project3D("zx"));
    result->Reset();
    for(unsigned ch=0;ch<cb_detector->GetNChannels();ch++) {
        if(cb_detector->IsIgnored(ch))
            continue;
        LOG(INFO) << "Fitting Channel=" << ch;
        h_timewalk->GetZaxis()->SetRange(ch,ch+1);
        stringstream ss_name;
        ss_name << "Ch" << ch << "_yx";
        TH2* proj = dynamic_cast<TH2*>(h_timewalk->Project3D(ss_name.str().c_str()));
        TObjArray aSlices;
        proj->FitSlicesY(nullptr, 0, -1, 0, "QNR", &aSlices);
        TH1D* means = dynamic_cast<TH1D*>(aSlices.At(1));
        for(Int_t x=0;x<means->GetNbinsX()+1;x++) {
            result->SetBinContent(x, ch+1, means->GetBinContent(x));
        }
    }
    c << result;
    c << endc;
}

CB_TimeWalk::CB_TimeWalk(
        const shared_ptr<expconfig::detector::CB>& cb,
        const shared_ptr<DataManager>& calmgr) :
    Module("CB_TimeWalk"),
    cb_detector(cb),
    calibrationManager(calmgr)
{
    for(unsigned ch=0;ch<cb_detector->GetNChannels();ch++) {
        timewalks.emplace_back(make_shared<gui::FitTimewalk>());
        timewalks.back()->SetDefaults(nullptr);
    }
}

CB_TimeWalk::~CB_TimeWalk()
{
}

void CB_TimeWalk::ApplyTo(clusterhits_t& sorted_clusterhits)
{
    // search for CB clusters
    const auto it_sorted_clusterhits = sorted_clusterhits.find(Detector_t::Type_t::CB);
    if(it_sorted_clusterhits == sorted_clusterhits.end())
        return;

    list< reconstruct::AdaptorTClusterHit >& clusterhits = it_sorted_clusterhits->second;

    auto it_clusterhit = clusterhits.begin();

    while(it_clusterhit != clusterhits.end()) {
        reconstruct::AdaptorTClusterHit& clusterhit = *it_clusterhit;
        clusterhit.Time -= timewalks[clusterhit.Hit->Channel]->Eval(clusterhit.Energy);
        ++it_clusterhit;
    }
}

unique_ptr<analysis::Physics> CB_TimeWalk::GetPhysicsModule() {
    return std_ext::make_unique<ThePhysics>(GetName(), cb_detector);
}

void CB_TimeWalk::GetGUIs(list<unique_ptr<gui::Manager_traits> >& guis) {
    guis.emplace_back(std_ext::make_unique<TheGUI>(GetName(), calibrationManager, cb_detector, timewalks));
}


vector<list<TID> > CB_TimeWalk::GetChangePoints() const
{
    return {calibrationManager->GetChangePoints(GetName())};
}

void CB_TimeWalk::Update(size_t, const TID& id)
{
    TCalibrationData cdata;
    if(!calibrationManager->GetData(GetName(), id, cdata))
        return;
    for(const TKeyValue<vector<double>>& kv : cdata.FitParameters) {
        if(kv.Key>=timewalks.size()) {
            LOG(ERROR) << "Ignoring too large key=" << kv.Key;
            continue;
        }
        timewalks[kv.Key]->Load(kv.Value);
    }
}


CB_TimeWalk::TheGUI::TheGUI(const string& basename,
                            const shared_ptr<DataManager>& calmgr,
                            const shared_ptr<expconfig::detector::CB>& cb,
                            std::vector< std::shared_ptr<gui::FitTimewalk> >& timewalks_) :
    gui::Manager_traits(basename),
    calibrationManager(calmgr),
    cb_detector(cb),
    timewalks(timewalks_)
{
}

string CB_TimeWalk::TheGUI::GetHistogramName() const
{
    return GetName()+"/timewalk";
}

unsigned CB_TimeWalk::TheGUI::GetNumberOfChannels() const
{
    return cb_detector->GetNChannels();
}

void CB_TimeWalk::TheGUI::InitGUI()
{
    c_fit = new gui::CalCanvas("canvas_fit", GetName());
    c_extra = new gui::CalCanvas("canvas_extra", GetName());

}

list<gui::CalCanvas*> CB_TimeWalk::TheGUI::GetCanvases() const
{
    return {c_fit, c_extra};
}

void CB_TimeWalk::TheGUI::StartRange(const interval<TID>& range)
{

    TCalibrationData cdata;
    if(!calibrationManager->GetData(GetName(), range.Start(), cdata)) {
        LOG(INFO) << " No previous data found";
        return;
    }
    for(const TKeyValue<vector<double>>& kv : cdata.FitParameters) {
        if(kv.Key>=timewalks.size()) {
            LOG(ERROR) << "Ignoring too large key=" << kv.Key;
            continue;
        }
        timewalks[kv.Key]->Load(kv.Value);
    }
}



gui::Manager_traits::DoFitReturn_t CB_TimeWalk::TheGUI::DoFit(TH1* hist, unsigned ch)
{
    if(cb_detector->IsIgnored(ch))
        return DoFitReturn_t::Skip;

    TH3* h_timewalk = dynamic_cast<TH3*>(hist);

    h_timewalk->GetZaxis()->SetRange(ch,ch+1);
    stringstream ss_name;
    ss_name << "Ch" << ch << "_yx";
    proj = dynamic_cast<TH2D*>(h_timewalk->Project3D(ss_name.str().c_str()));
    TObjArray aSlices;
    proj->FitSlicesY(nullptr, 0, -1, 0, "QNR", &aSlices);
    means = dynamic_cast<TH1D*>(aSlices.At(1)->Clone()); // important to use Clone here!

    timewalks[ch]->Fit(means);

    last_timewalk = timewalks[ch]; // remember for display fit

    // always request display
    return DoFitReturn_t::Display;
}

void CB_TimeWalk::TheGUI::DisplayFit()
{
    c_fit->Show(means, last_timewalk.get());

    c_extra->cd();
    proj->Draw("colz");
}

void CB_TimeWalk::TheGUI::StoreFit(unsigned channel)
{
    // the fit parameters contain the timewalk correction
    // and since we use pointers, the item in timewalks is already updated

    LOG(INFO) << "Stored Ch=" << channel;
    c_fit->Clear();
    c_fit->Update();
    c_extra->Clear();
    c_extra->Update();
}

bool CB_TimeWalk::TheGUI::FinishRange()
{
   return true;
}

void CB_TimeWalk::TheGUI::StoreFinishRange(const interval<TID>& range)
{
    TCalibrationData cdata(
                "Unknown", /// \todo get static information about author/comment?
                "No Comment",
                time(nullptr),
                GetName(),
                range.Start(),
                range.Stop()
                );

    // fill fit parameters
    for(unsigned ch=0;ch<cb_detector->GetNChannels();ch++) {
        const shared_ptr<gui::FitTimewalk>& func = timewalks[ch];
        cdata.FitParameters.emplace_back(ch, func->Save());
    }

    calibrationManager->Add(cdata);
    LOG(INFO) << "Added TCalibrationData: " << cdata;
}

