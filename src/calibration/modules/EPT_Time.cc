#include "EPT_Time.h"

#include "base/Logger.h"

#include <cstdint>

using namespace std;
using namespace ant;
using namespace ant::calibration;

EPT_Time::EPT_Time(shared_ptr<expconfig::detector::EPT> ept,
                   shared_ptr<DataManager> calmgr,
                   std::map<expconfig::detector::EPT::Sector_t, Calibration::Converter::ptr_t> converters,
                   double defaultOffset,
                   shared_ptr<gui::PeakingFitFunction> fitFunction,
                   const interval<double>& timeWindow
                     ) :
    Time(ept,
         calmgr,
         nullptr, // do not set any converter by default
         defaultOffset,
         fitFunction,
         timeWindow
         )
{
    // set each converter individually depending on the EPT sector
    // one may use also any other property of the dectector,
    // another example is TAPS_Time, which handles PbWO4 differently than BaF2
    for(unsigned ch=0;ch<ept->GetNChannels();ch++) {
        /// \todo better check if the given converters actually
        /// contain the sector, otherwise nasty segfaults will happen
        /// during reconstruction (and that's the Acqu and not the Ant way :)
        Converters[ch] = converters[ept->GetSector(ch)];
    }
}
