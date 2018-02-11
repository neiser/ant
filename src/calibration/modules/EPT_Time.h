#pragma once

#include "Time.h"
#include "expconfig/detectors/EPT.h"

#include <memory>

namespace ant {

namespace calibration {

class EPT_Time : public Time
{

public:
    EPT_Time(std::shared_ptr<expconfig::detector::EPT> ept,
             std::shared_ptr<DataManager> calmgr,
             std::map<expconfig::detector::EPT::Sector_t, Calibration::Converter::ptr_t> converters,
             double defaultOffset,
             std::shared_ptr<gui::PeakingFitFunction> fitFunction,
             const interval<double>& timeWindow
             );
};

}}  // namespace ant::calibration
