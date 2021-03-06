#include "ClusterCorrection.h"

#include "calibration/DataManager.h"
#include "tree/TCalibrationData.h"
#include "tree/TDetectorReadHit.h"
#include "tree/TCluster.h"
#include "base/Logger.h"
#include "base/std_ext/math.h"
#include "base/ClippedInterpolatorWrapper.h"
#include "detail/TH2Storage.h"

#include "TH2.h"

#include <list>
#include <cmath>

#include "TRandom.h"



using namespace std;
using namespace ant;
using namespace ant::calibration;
using namespace ant::std_ext;

ClusterCorrection::ClusterCorrection(std::shared_ptr<ClusterDetector_t> det,
                           const std::string &Name, const Filter_t Filter,
                           std::shared_ptr<DataManager> calmgr
                           ) :
    Calibration::BaseModule(
        std_ext::formatter()
        << Detector_t::ToString(det->Type)
        << "_"
        << Name
           ),
    DetectorType(det->Type),
    filter(Filter),
    calibrationManager(calmgr),
    interpolator(nullptr)
{}

ClusterCorrection::~ClusterCorrection()
{
}


struct ClusterCorrection::Interpolator {

    double Get(const double E, const double theta) const {
        return interp->GetPoint(E, cos(theta));
    }

    Interpolator(TH2D* h) { CleanupHistogram(h); CreateInterpolators(h); }

protected:

    std::unique_ptr<ClippedInterpolatorWrapper> interp;

    void CreateInterpolators(TH2D* hist) {
        interp = std_ext::make_unique<ClippedInterpolatorWrapper>(
                     ClippedInterpolatorWrapper::makeInterpolator(hist)
                     );
    }

    static void CleanupHistogram(TH2* hist) {

        auto check = [] (const double x) {
            return isfinite(x) && x >= 0.0;
        };

        for(int y = 1; y<=hist->GetNbinsY(); ++y) {
            for(int x = 1; x<=hist->GetNbinsX(); ++x) {
                if(!check(hist->GetBinContent(x,y))) {
                    for(int dx=1; dx<=hist->GetNbinsX();++dx) {
                        if(x-dx >= 1 && check(hist->GetBinContent(x-dx,y))) {
                            hist->SetBinContent(x,y, hist->GetBinContent(x-dx,y));
                            break;
                        }
                        if(x+dx <= hist->GetNbinsX() && check(hist->GetBinContent(x+dx,y))) {
                            hist->SetBinContent(x,y, hist->GetBinContent(x+dx,y));
                            break;
                        }
                    }
                }
            }
        }
    }
};

void ClusterCorrection::ApplyTo(clusters_t& clusters)
{

    if(interpolator) {

        const auto& entry = clusters.find(DetectorType);

        if(entry != clusters.end()) {

            for(auto& cluster : entry->second) {

                ApplyTo(cluster);

                if(cluster.Energy < 0.0)
                    cluster.Energy = 0.0;
            }
        }
    }
}


std::list<Updateable_traits::Loader_t> ClusterCorrection::GetLoaders()
{

    return {
        [this] (const TID& currPoint, TID& nextChangePoint) {

            const bool isMC    = currPoint.isSet(TID::Flags_t::MC);

            TCalibrationData cdata;

            const bool loadOK = calibrationManager->GetData(GetName(), currPoint, cdata, nextChangePoint);

            if((isMC && filter == Filter_t::Data)
                  || (!isMC  && filter == Filter_t::MC)) {
                this->interpolator = nullptr;
                return;
            }

            if(!loadOK) {
                LOG(WARNING) << "No data found for " << GetName();
                this->interpolator = nullptr;
                return;
            }

            auto hist = detail::TH2Storage::Decode(cdata);

            this->interpolator = std_ext::make_unique<Interpolator>(hist);

            delete hist;
        }
    };
}

void ClusterSmearing::ApplyTo(TCluster &cluster)
{
    const auto sigma  = interpolator->Get(cluster.Energy, cluster.Position.Theta());
    cluster.Energy    = gRandom->Gaus(cluster.Energy, sigma);
}

void ClusterECorr::ApplyTo(TCluster &cluster)
{
    const auto factor  = interpolator->Get(cluster.Energy, cluster.Hits.size());
    cluster.Energy    *= factor;
}
