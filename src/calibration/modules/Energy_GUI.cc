#include "Energy_GUI.h"

#include "calibration/DataManager.h"
#include "calibration/gui/CalCanvas.h"
#include "calibration/fitfunctions/FitGausPol1.h"
#include "calibration/fitfunctions/FitLandauExpo.h"
#include "calibration/fitfunctions/FitWeibullLandauPol1.h"
#include "calibration/fitfunctions/FitVetoBand.h"

#include "tree/TCalibrationData.h"

#include "base/math_functions/Linear.h"
#include "base/std_ext/math.h"
#include "base/std_ext/string.h"
#include "base/Logger.h"

#include "TH2.h"
#include "TH3.h"
#include "TF1.h"

#include "TGNumberEntry.h"

using namespace std;
using namespace ant;
using namespace ant::calibration;
using namespace ant::calibration::energy;

GUI_CalibType::GUI_CalibType(const string& basename, OptionsPtr opts,
                                     CalibType& type,
                                     const shared_ptr<DataManager>& calmgr,
                                     const detector_ptr_t& detector_,
                                     Calibration::AddMode_t mode) :
    gui::CalibModule_traits(basename),
    options(opts),
    calibType(type),
    calibrationManager(calmgr),
    detector(detector_),
    addMode(mode)
{}

string GUI_CalibType::GetName() const
{
    // serves as the CalibrationID for the manager
    return  CalibModule_traits::GetName()+"_"+calibType.Name;
}

shared_ptr<TH1> GUI_CalibType::GetHistogram(const WrapTFile& file) const
{
    // histogram name created by the specified Physics class
    return file.GetSharedHist<TH1>(options->Get<string>("HistogramPath", CalibModule_traits::GetName()) + "/"+calibType.HistogramName);
}

unsigned GUI_CalibType::GetNumberOfChannels() const
{
    return detector->GetNChannels();
}

void GUI_CalibType::InitGUI(gui::ManagerWindow_traits& window) {
    window.AddCheckBox("Ignore prev fit params", IgnorePreviousFitParameters);
    window.AddCheckBox("Use params from prev slice", UsePreviousSliceParams);
}

void GUI_CalibType::StartSlice(const interval<TID>& range)
{
    // clear previous values from slice first
    // then calibType.Get(ch) will return default value
    calibType.Values.clear();
    std::vector<double> values(GetNumberOfChannels());
    for(size_t ch=0; ch<values.size(); ++ch) {
        values.at(ch) = calibType.Get(ch);
    }

    TCalibrationData cdata;
    if(calibrationManager->GetData(GetName(), range.Start(), cdata)) {
        for(const TKeyValue<double>& kv : cdata.Data) {
            if(kv.Key>=GetNumberOfChannels()) {
                LOG(WARNING) << "Ignoring too large key " << kv.Key << " in TCalibrationData";
                continue;
            }
            values.at(kv.Key) = kv.Value;
        }
        LOG(INFO) << GetName() << ": Loaded previous values from database";

        // fill the map of fitparameters
        if(fitParameters.empty() || !UsePreviousSliceParams) {
            for(const TKeyValue<vector<double>>& kv : cdata.FitParameters) {
                if(kv.Key>=GetNumberOfChannels()) {
                    LOG(WARNING) << "Ignoring too large key " << kv.Key << " in TCalibrationData fit parameters";
                    continue;
                }
                // do not use at() as kv.Key might not yet exist in map
                fitParameters[kv.Key] = kv.Value;
            }
            LOG(INFO) << GetName() << ": Loaded previous fit parameter from database";
        }
        else if(!fitParameters.empty()) {
            LOG(INFO) << GetName() << ": Using fit parameters from previous slice";
        }
    }
    else {
        LOG(INFO) << GetName() << ": No previous values found, built from default value";
    }

    calibType.Values = values;

    // save a copy for comparison at finish stage
    previousValues = calibType.Values;

}

void GUI_CalibType::StoreFinishSlice(const interval<TID>& range)
{
    TCalibrationData cdata(
                GetName(),
                range.Start(),
                range.Stop()
                );

    std::vector<double>& values = calibType.Values;

    // fill data
    for(unsigned ch=0;ch<values.size();ch++) {
        cdata.Data.emplace_back(ch, values[ch]);
    }

    // fill fit parameters (if any)
    for(const auto& it_map : fitParameters) {
        const unsigned ch = it_map.first;
        const vector<double>& params = it_map.second;
        cdata.FitParameters.emplace_back(ch, params);
    }

    calibrationManager->Add(cdata, addMode);
}

GUI_Pedestals::GUI_Pedestals(const string& basename,
        OptionsPtr options,
        CalibType& type,
        const std::shared_ptr<DataManager>& calmgr,
        const detector_ptr_t& detector,
        shared_ptr<gui::PeakingFitFunction> fitfunction) :
    GUI_CalibType(basename, options, type, calmgr, detector, Calibration::AddMode_t::RightOpen),
    func(fitfunction)
{

}

void GUI_Pedestals::InitGUI(gui::ManagerWindow_traits& window)
{
    GUI_CalibType::InitGUI(window);
    canvas = window.AddCalCanvas();
}

gui::CalibModule_traits::DoFitReturn_t GUI_Pedestals::DoFit(const TH1& hist, unsigned channel)
{
    if(detector->IsIgnored(channel))
        return DoFitReturn_t::Skip;

    auto& hist2 = dynamic_cast<const TH2&>(hist);

    h_projection = hist2.ProjectionX("h_projection",channel+1,channel+1);

    func->SetDefaults(h_projection);
    const auto it_fit_param = fitParameters.find(channel);
    if(it_fit_param != fitParameters.end() && !IgnorePreviousFitParameters) {
        VLOG(5) << "Loading previous fit parameters for channel " << channel;
        func->Load(it_fit_param->second);
    }

    for(size_t i=0;i<5;i++)
        func->Fit(h_projection);

    /// \todo implement automatic stop if fit failed?

    // goto next channel
    return DoFitReturn_t::Next;
}

void GUI_Pedestals::DisplayFit()
{
    canvas->Show(h_projection, func.get());
}

void GUI_Pedestals::StoreFit(unsigned channel)
{

    const double oldValue = previousValues[channel];
    const double newValue = func->GetPeakPosition();

    calibType.Values[channel] = newValue;

    const double relative_change = 100*(newValue/oldValue-1);

    LOG(INFO) << "Stored Ch=" << channel << ":  "
              <<" Pedestal changed " << oldValue << " -> " << newValue
              << " (" << relative_change << " %)";


    // don't forget the fit parameters
    fitParameters[channel] = func->Save();
}

bool GUI_Pedestals::FinishSlice()
{
    return false;
}

struct FitProtonPeak : gui::FitGausPol1 {
    virtual void SetDefaults(TH1 *hist) override {

        const auto range = GetRange();

        const auto startbin = hist->FindBin(range.Start());
        const auto stopbin  = hist->FindBin(range.Stop());

        // try to autodedect maximum within fit range
        double maxx = range.Center();
        double maxy = -std_ext::inf;
        for(int i=startbin; i<=stopbin; ++i) {
            const auto v = hist->GetBinContent(i);
            if(v > maxy) {
                maxy = v;
                maxx = hist->GetBinCenter(i);
            }
        }

        if(!isfinite(maxy))
           maxy = hist->GetMaximum();

        // linear background
        const math::LineFct bg({hist->GetBinCenter(startbin), hist->GetBinContent(startbin)},
                               {hist->GetBinCenter(stopbin),  hist->GetBinContent(stopbin)} );

        // amplitude
        func->SetParameter(0, maxy - bg(maxx));

        // x0
        func->SetParameter(1, maxx);

        // sigma
        func->SetParameter(2, 1.5);

        // pol1
        func->SetParameter(3, bg.b);
        func->SetParameter(4, bg.m);

        Sync();
    }

};

GUI_Banana::GUI_Banana(const string& basename,
                               OptionsPtr options,
                               CalibType& type,
                               const std::shared_ptr<DataManager>& calmgr,
                               const detector_ptr_t& detector,
                               const interval<double>& projectionrange,
                               const double proton_peak_mc_pos
                               ) :
    GUI_CalibType(basename, options, type, calmgr, detector),
    func(make_shared<FitProtonPeak>()),
    projection_range(projectionrange),
    proton_peak_mc(proton_peak_mc_pos),
    full_hist_name(
            options->Get<string>("HistogramPath", CalibModule_traits::GetName())
            + "/"
            + options->Get<string>("HistogramName", "Bananas"))
{

}

std::shared_ptr<TH1> GUI_Banana::GetHistogram(const WrapTFile& file) const
{
    return file.GetSharedHist<TH1>(full_hist_name);
}

void GUI_Banana::InitGUI(gui::ManagerWindow_traits& window)
{
    GUI_CalibType::InitGUI(window);
    window.AddNumberEntry("Chi2/NDF limit for autostop", AutoStopOnChi2);

    c_fit = window.AddCalCanvas();
    c_extra = window.AddCalCanvas();


    h_relative = new TH1D("h_relative","Relative change from previous gains",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_relative->SetXTitle("Channel Number");
    h_relative->SetYTitle("Relative change / %");
}

gui::CalibModule_traits::DoFitReturn_t GUI_Banana::DoFit(const TH1& hist, unsigned ch)
{
    if(detector->IsIgnored(ch))
        return DoFitReturn_t::Skip;

    auto& h_bananas = dynamic_cast<const TH3&>(hist);
    h_bananas.GetZaxis()->SetRange(ch+1,ch+1);
    banana = dynamic_cast<TH2D*>(h_bananas.Project3D("yx"));
    auto xaxis = banana->GetXaxis();
    h_projection = dynamic_cast<TH1D*>(banana->ProjectionY(
                                           "_py",
                                           xaxis->FindFixBin(projection_range.Start()),
                                           xaxis->FindFixBin(projection_range.Stop())
                                           )
                                       );

    if(h_projection->GetNbinsX() > 100) {
        const auto grp = int(std::ceil(h_projection->GetNbinsX()/100.0));
        h_projection->Rebin(grp);
    }

    // stop at empty histograms
    if(h_projection->GetEntries()==0)
        return DoFitReturn_t::Display;

    func->SetRange(interval<double>(0.5,6));
    func->SetDefaults(h_projection);
    const auto it_fit_param = fitParameters.find(ch);
    if(it_fit_param != fitParameters.end() && !IgnorePreviousFitParameters) {
        VLOG(5) << "Loading previous fit parameters for channel " << ch;
        func->Load(it_fit_param->second);
    }

    auto fit_loop = [this] (size_t retries) {
        do {
            func->Fit(h_projection);
            VLOG(5) << "Chi2/dof = " << func->Chi2NDF();
            if(func->Chi2NDF() < AutoStopOnChi2) {
                return true;
            }
            retries--;
        }
        while(retries>0);
        return false;
    };

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // reached maximum retries without good chi2
    LOG(INFO) << "Chi2/dof = " << func->Chi2NDF();
    return DoFitReturn_t::Display;
}

void GUI_Banana::DisplayFit()
{
    c_fit->Show(h_projection, func.get());

    c_extra->cd();
    banana->Draw("colz");
}

void GUI_Banana::StoreFit(unsigned channel)
{
    const double oldValue = previousValues[channel];

    const double protonpeak = func->GetPeakPosition();

    const double newValue = oldValue * proton_peak_mc / protonpeak;

    calibType.Values[channel] = newValue;

    const double relative_change = 100*(newValue/oldValue-1);

    LOG(INFO) << "Stored Ch=" << channel << ": ProtonPeak " << protonpeak
              << " MeV,  gain changed " << oldValue << " -> " << newValue
              << " (" << relative_change << " %)";


    // don't forget the fit parameters
    fitParameters[channel] = func->Save();

    h_relative->SetBinContent(channel+1, relative_change);

}

bool GUI_Banana::FinishSlice()
{
    c_extra->Clear();
    c_fit->Clear();

    c_fit->cd();
    h_relative->SetStats(false);
    h_relative->Draw("P");

    return true;
}


GUI_MIP::GUI_MIP(const string& basename,
                         OptionsPtr options,
                         CalibType& type,
                         const std::shared_ptr<DataManager>& calmgr,
                         const detector_ptr_t& detector,
                         const double peak_mc_pos
                         ) :
    GUI_CalibType(basename, options, type, calmgr, detector),
    func(make_shared<gui::FitLandauExpo>()),
    peak_mc(peak_mc_pos),
    full_hist_name(
            options->Get<string>("HistogramPath", CalibModule_traits::GetName())
            + "/"
            + options->Get<string>("HistogramName", "MIP"))
{

}

std::shared_ptr<TH1> GUI_MIP::GetHistogram(const WrapTFile& file) const
{
    return file.GetSharedHist<TH1>(full_hist_name);
}

void GUI_MIP::InitGUI(gui::ManagerWindow_traits& window)
{
    GUI_CalibType::InitGUI(window);
    window.AddNumberEntry("Chi2/NDF limit for autostop", AutoStopOnChi2);

    canvas = window.AddCalCanvas();

    h_peaks = new TH1D("h_peaks","Peak positions",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_peaks->SetXTitle("Channel Number");
    h_peaks->SetYTitle("Minimum Ionizing Peak / MeV");
    h_relative = new TH1D("h_relative","Relative change from previous gains",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_relative->SetXTitle("Channel Number");
    h_relative->SetYTitle("Relative change / %");
}

gui::CalibModule_traits::DoFitReturn_t GUI_MIP::DoFit(const TH1& hist, unsigned ch)
{
    if(detector->IsIgnored(ch))
        return DoFitReturn_t::Skip;

    auto& hist2 = dynamic_cast<const TH2&>(hist);
    h_projection = hist2.ProjectionX("h_projection",ch+1,ch+1);

    // stop at empty histograms
    if(h_projection->GetEntries()==0)
        return DoFitReturn_t::Display;

    auto range = interval<double>(0.5,7);

    func->SetDefaults(h_projection);
    func->SetRange(range);
    const auto it_fit_param = fitParameters.find(ch);
    if(it_fit_param != fitParameters.end() && !IgnorePreviousFitParameters && false) {
        VLOG(5) << "Loading previous fit parameters for channel " << ch;
        func->Load(it_fit_param->second);
    }
    else {
        func->FitSignal(h_projection);
    }

    auto fit_loop = [this] (size_t retries) {
        do {
            func->Fit(h_projection);
            VLOG(5) << "Chi2/dof = " << func->Chi2NDF();
            if(func->Chi2NDF() < AutoStopOnChi2) {
                return true;
            }
            retries--;
        }
        while(retries>0);
        return false;
    };

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // try with defaults and signal fit
    func->SetDefaults(h_projection);
    func->SetRange(range);
    func->FitSignal(h_projection);

    if(fit_loop(5))
        return DoFitReturn_t::Next;


    // reached maximum retries without good chi2
    LOG(INFO) << "Chi2/dof = " << func->Chi2NDF();
    return DoFitReturn_t::Display;
}

void GUI_MIP::DisplayFit()
{
    canvas->Show(h_projection, func.get());
}

void GUI_MIP::StoreFit(unsigned channel)
{
    const double oldValue = previousValues[channel];
    const double peak = func->GetPeakPosition();
    const double newValue = oldValue * peak_mc / peak;

    calibType.Values[channel] = newValue;

    const double relative_change = 100*(newValue/oldValue-1);

    LOG(INFO) << "Stored Ch=" << channel << ": PeakPosition " << peak
              << " MeV,  gain changed " << oldValue << " -> " << newValue
              << " (" << relative_change << " %)";


    // don't forget the fit parameters
    fitParameters[channel] = func->Save();

    h_peaks->SetBinContent(channel+1, peak);
    h_relative->SetBinContent(channel+1, relative_change);

}

bool GUI_MIP::FinishSlice()
{
    canvas->Clear();
    canvas->Divide(1,2);

    canvas->cd(1);
    h_peaks->SetStats(false);
    h_peaks->Draw("P");
    canvas->cd(2);
    h_relative->SetStats(false);
    h_relative->Draw("P");

    return true;
}


GUI_HEP::GUI_HEP(const string& basename,
                         OptionsPtr options,
                         CalibType& type,
                         const std::shared_ptr<DataManager>& calmgr,
                         const detector_ptr_t& detector,
                         const double proton_peak_mc_pos
                         ) :
    GUI_CalibType(basename, options, type, calmgr, detector),
    func(make_shared<gui::FitWeibullLandauPol1>()),
    proton_peak_mc(proton_peak_mc_pos),
    full_hist_name(
            options->Get<string>("HistogramPath", CalibModule_traits::GetName())
            + "/"
            + options->Get<string>("HistogramName", "projections_hep"))
{

}

std::shared_ptr<TH1> GUI_HEP::GetHistogram(const WrapTFile& file) const
{
    return file.GetSharedHist<TH1>(full_hist_name);
}

void GUI_HEP::InitGUI(gui::ManagerWindow_traits& window)
{
    GUI_CalibType::InitGUI(window);
    window.AddNumberEntry("Chi2/NDF limit for autostop", AutoStopOnChi2);

    canvas = window.AddCalCanvas();

    h_peaks = new TH1D("h_peaks","Peak positions",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_peaks->SetXTitle("Channel Number");
    h_peaks->SetYTitle("High Energy Proton Peak / MeV");
    h_relative = new TH1D("h_relative","Relative change from previous gains",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_relative->SetXTitle("Channel Number");
    h_relative->SetYTitle("Relative change / %");
}

gui::CalibModule_traits::DoFitReturn_t GUI_HEP::DoFit(const TH1& hist, unsigned ch)
{
    if(detector->IsIgnored(ch))
        return DoFitReturn_t::Skip;

    auto& hist2 = dynamic_cast<const TH2&>(hist);
    h_projection = hist2.ProjectionX("h_projection",ch+1,ch+1);

    // stop at empty histograms
    if(h_projection->GetEntries()==0)
        return DoFitReturn_t::Display;

    //auto range = interval<double>(1,9);
    auto range = interval<double>(.8,9.5);

    func->SetDefaults(h_projection);
    func->SetRange(range);
    const auto it_fit_param = fitParameters.find(ch);
    if(it_fit_param != fitParameters.end() && !IgnorePreviousFitParameters && false) {
        VLOG(5) << "Loading previous fit parameters for channel " << ch;
        func->Load(it_fit_param->second);
    }
    else {
        func->FitSignal(h_projection);
    }

    auto fit_loop = [this] (size_t retries) {
        do {
            func->Fit(h_projection);
            VLOG(5) << "Chi2/dof = " << func->Chi2NDF();
            if(func->Chi2NDF() < AutoStopOnChi2) {
                return true;
            }
            retries--;
        }
        while(retries>0);
        return false;
    };

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // try with defaults ...
    func->SetDefaults(h_projection);
    func->Fit(h_projection);

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // ... and with defaults and first a signal only fit ...
    func->SetDefaults(h_projection);
    func->FitSignal(h_projection);

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // ... and as a last resort background, signal and a few last fit tries
    func->SetDefaults(h_projection);
    func->FitBackground(h_projection);
    func->Fit(h_projection);
    func->FitSignal(h_projection);

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // reached maximum retries without good chi2
    LOG(INFO) << "Chi2/dof = " << func->Chi2NDF();
    return DoFitReturn_t::Display;
}

void GUI_HEP::DisplayFit()
{
    canvas->Show(h_projection, func.get());
}

void GUI_HEP::StoreFit(unsigned channel)
{
    const double oldValue = previousValues[channel];
    const double peak = func->GetPeakPosition();
    const double newValue = oldValue * proton_peak_mc / peak;

    calibType.Values[channel] = newValue;

    const double relative_change = 100*(newValue/oldValue-1);

    LOG(INFO) << "Stored Ch=" << channel << ": PeakPosition " << peak
              << " MeV,  gain changed " << oldValue << " -> " << newValue
              << " (" << relative_change << " %)";


    // don't forget the fit parameters
    fitParameters[channel] = func->Save();

    h_peaks->SetBinContent(channel+1, peak);
    h_relative->SetBinContent(channel+1, relative_change);

}

bool GUI_HEP::FinishSlice()
{
    canvas->Clear();
    canvas->Divide(1,2);

    canvas->cd(1);
    h_peaks->SetStats(false);
    h_peaks->Draw("P");
    canvas->cd(2);
    h_relative->SetStats(false);
    h_relative->Draw("P");

    return true;
}


GUI_BananaSlices::GUI_BananaSlices(const string& basename,
                         OptionsPtr options,
                         CalibType& type,
                         const std::shared_ptr<DataManager>& calmgr,
                         const detector_ptr_t& detector,
                         const interval<double>& fitrange
                         ) :
    GUI_CalibType(basename, options, type, calmgr, detector),
    func(make_shared<gui::FitVetoBand>()),
    fit_range(fitrange),
    full_hist_name(
            options->Get<string>("HistogramPath", CalibModule_traits::GetName())
            + "/"
            + options->Get<string>("HistogramName", "dEvE_all_combined"))
{
    slicesY_gaus = new TF1("slicesY_gaus","gaus");

    LOG(INFO) << "Initialized fitting of Veto bananas";
    LOG(WARNING) << "Please make sure to set a fixed energy fitting range"
                 << " via the GUI number fields and keep it for all channels!";
}

std::shared_ptr<TH1> GUI_BananaSlices::GetHistogram(const WrapTFile& file) const
{
    return file.GetSharedHist<TH1>(full_hist_name);
}

void GUI_BananaSlices::InitGUI(gui::ManagerWindow_traits& window)
{
    GUI_CalibType::InitGUI(window);

    c_fit = window.AddCalCanvas();
    c_extra = window.AddCalCanvas();

    window.AddNumberEntry("Lower Energy Limit for fit function",fit_range.Start(),[this] (const TGNumberEntry& e) {
        fit_range.Start() = e.GetNumber();
        func->SetRange(fit_range);
        c_fit->UpdateMe();
    });

    window.AddNumberEntry("Upper Energy Limit for fit function",fit_range.Stop(),[this] (const TGNumberEntry& e) {
        fit_range.Stop() = e.GetNumber();
        func->SetRange(fit_range);
        c_fit->UpdateMe();
    });

    window.AddNumberEntry("Chi2/NDF limit for autostop", AutoStopOnChi2);
    window.AddNumberEntry("SlicesYEntryCut", slicesY_entryCut);
    window.AddNumberEntry("SlicesYIQRFactor low  (outlier detection)", slicesY_IQRFactor_lo);
    window.AddNumberEntry("SlicesYIQRFactor high (outlier detection)", slicesY_IQRFactor_hi);

    h_vals = new TH1D("h_vals","Energy values from Veto band",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_vals->SetXTitle("Channel Number");
    h_vals->SetYTitle("Calculated Veto Energy / MeV");
    h_relative = new TH1D("h_relative","Relative change from previous gains",GetNumberOfChannels(),0,GetNumberOfChannels());
    h_relative->SetXTitle("Channel Number");
    h_relative->SetYTitle("Relative change / %");
}

// copied and adapted from TH2::FitSlicesY/DoFitSlices
TH1D* MyFitSlicesY(const TH2* h, TF1 *f1, Int_t cut, double IQR_range_lo, double IQR_range_hi)
{
    TAxis& outerAxis = *h->GetXaxis();
    Int_t nbins  = outerAxis.GetNbins();

    Int_t npar = f1->GetNpar();

    //Create one histogram for each function parameter

    char *name   = new char[2000];
    char *title  = new char[2000];
    const TArrayD *bins = outerAxis.GetXbins();

    snprintf(name,2000,"%s_Mean",h->GetName());
    snprintf(title,2000,"Fitted value of Mean");
    delete gDirectory->FindObject(name);
    TH1D *hmean = nullptr;
    if (bins->fN == 0) {
        hmean = new TH1D(name,title, nbins, outerAxis.GetXmin(), outerAxis.GetXmax());
    } else {
        hmean = new TH1D(name,title, nbins,bins->fArray);
    }
    hmean->GetXaxis()->SetTitle(outerAxis.GetTitle());

    // Loop on all bins in Y, generate a projection along X
    struct value_t {
        int Bin;
        double Value;
        double Error;
    };
    std::vector<value_t> means;
    for (Int_t bin=0;bin<=nbins+1;bin++) {
        TH1D *hp = h->ProjectionY("_temp",bin,bin,"e");
        if (hp == 0) continue;
        Long64_t nentries = Long64_t(hp->GetEntries());
        if (nentries == 0 || nentries < cut) {delete hp; continue;}


        const double max_pos = hp->GetXaxis()->GetBinCenter(hp->GetMaximumBin());
        const double sigma = hp->GetRMS();

        // setting meaningful start parameters and limits
        // is crucial for a good fit!
        f1->SetParameter(0, hp->GetMaximum());
        f1->SetParLimits(0, 0, hp->GetMaximum());
        f1->SetParLimits(1, max_pos-4*sigma, max_pos+4*sigma);
        f1->SetParameter(1, max_pos);
        f1->SetParLimits(2, 0, 60);
        f1->SetParameter(2, sigma); // set sigma
        f1->SetRange(max_pos-4*sigma, max_pos+4*sigma);

        hp->Fit(f1,"QBNR"); // B important for limits!

        Int_t npfits = f1->GetNumberFitPoints();
        if (npfits > npar && npfits >= cut)
            means.push_back({bin, f1->GetParameter(1), f1->GetParError(1)});

        delete hp;
    }
    delete [] name;
    delete [] title;

    // get some robust estimate of mean errors,
    // to kick out strange outliers
    std_ext::IQR iqr;
    for(const auto& mean : means) {
        iqr.Add(mean.Error);
    }

    auto valid_range = iqr.GetN()==0 ?
                           interval<double>(-std_ext::inf, std_ext::inf) :
                           interval<double>(iqr.GetMedian() - IQR_range_lo*iqr.GetIQR(),
                                            iqr.GetMedian() + IQR_range_hi*iqr.GetIQR());

    for(const auto& mean : means) {
        if(!valid_range.Contains(mean.Error))
            continue;
        hmean->Fill(outerAxis.GetBinCenter(mean.Bin),mean.Value);
        hmean->SetBinError(mean.Bin,mean.Error);
    }
    return hmean;
}

gui::CalibModule_traits::DoFitReturn_t GUI_BananaSlices::DoFit(const TH1& hist, unsigned ch)
{
    if(detector->IsIgnored(ch))
        return DoFitReturn_t::Skip;

    //auto& h_vetoband = dynamic_cast<const TH3&>(hist);

    //h_vetoband.GetZaxis()->SetRange(ch+1,ch+1);
    //proj = dynamic_cast<TH2D*>(h_vetoband.Project3D("yx"));
    auto& proj = dynamic_cast<const TH2&>(hist);

    means = MyFitSlicesY(&proj, slicesY_gaus,
                         slicesY_entryCut, slicesY_IQRFactor_lo, slicesY_IQRFactor_hi);
    //means->SetMinimum(proj.GetYaxis()->GetXmin());
    //means->SetMaximum(proj.GetYaxis()->GetXmax());

    func->SetDefaults(means);
    func->SetRange(fit_range);
    const auto it_fit_param = fitParameters.find(ch);
    if(it_fit_param != fitParameters.end()) {
        VLOG(5) << "Loading previous fit parameters for channel " << ch;
        func->Load(it_fit_param->second);
        func->SetRange(fit_range);
    }


    auto fit_loop = [this] (size_t retries) {
        do {
            func->Fit(means);
            VLOG(5) << "Chi2/dof = " << func->Chi2NDF();
            if(func->Chi2NDF() < AutoStopOnChi2) {
                return true;
            }
            retries--;
        }
        while(retries>0);
        return false;
    };

    if(fit_loop(5))
        return DoFitReturn_t::Next;

    // reached maximum retries without good chi2
    LOG(INFO) << "Chi2/dof = " << func->Chi2NDF();
    return DoFitReturn_t::Display;
}

void GUI_BananaSlices::DisplayFit()
{
    c_fit->Show(means, func.get(), true);

    //proj->DrawCopy("colz");
    c_extra->cd();
    func->Draw();
}

void GUI_BananaSlices::StoreFit(unsigned channel)
{
    const double energy = fit_range.Stop();
    const double oldValue = previousValues[channel];
    const double val = func->Eval(energy);
    const double ref = func->EvalReference(energy);
    const double newValue = oldValue * ref/val;

    calibType.Values[channel] = newValue;

    const double relative_change = 100*(newValue/oldValue-1);

    LOG(INFO) << "Stored Ch=" << channel << ": Energy value at " << energy
              << " MeV: " << val << " MeV, reference: " << ref << " MeV"
              << " ;  gain changed " << oldValue << " -> " << newValue
              << " (" << relative_change << " %)";


    // don't forget the fit parameters
    fitParameters[channel] = func->Save();

    h_vals->SetBinContent(channel+1, val);
    h_relative->SetBinContent(channel+1, relative_change);

    //LOG(INFO) << "Stored Ch=" << channel << " Parameters: " << fitParameters[channel];
}

bool GUI_BananaSlices::FinishSlice()
{
    // don't request stop...
    return false;

//    canvas->Clear();
//    canvas->Divide(1,2);

//    canvas->cd(1);
//    h_vals->SetStats(false);
//    h_vals->Draw("P");
//    canvas->cd(2);
//    h_relative->SetStats(false);
//    h_relative->Draw("P");

//    return true;
}
