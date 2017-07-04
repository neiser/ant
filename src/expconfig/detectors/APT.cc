#include "APT.h"
#include "detail/apt_elements.h"

#include "tree/TID.h"
#include "base/std_ext/math.h"

#include <cassert>
#include <cmath> // provides M_PI

using namespace std;
using namespace ant;
using namespace ant::expconfig::detector;

void APT::BuildMappings(vector<UnpackerAcquConfig::hit_mapping_t>& hit_mappings,
                        vector<UnpackerAcquConfig::scaler_mapping_t>&) const
{
    for(const Element_t& element : elements)
    {
        hit_mappings.emplace_back(Type,
                                  Channel_t::Type_t::Integral,
                                  element.Channel,
                                  element.ADC);

        hit_mappings.emplace_back(Type,
                                  Channel_t::Type_t::Timing,
                                  element.Channel,
                                  element.TDC);
    }
}

void APT::InitElements()
{
    assert(elements.size() == 16);
    for(size_t i=0; i<elements.size();i++) {
        Element_t& element = elements[i];
        if(element.Channel != i)
            throw Exception("PID element channels not in correct order");
    }
}
