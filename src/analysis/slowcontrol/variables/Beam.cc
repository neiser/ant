#include "Beam.h"

#include "SlowControlProcessors.h"

#include "expconfig/ExpConfig.h"

#include "base/Logger.h"

using namespace std;
using namespace ant::analysis::slowcontrol;
using namespace ant::analysis::slowcontrol::variable;


list<Variable::ProcessorPtr> Beam::GetNeededProcessors() const
{
    return {Processors::Beampolmon,Processors::Beam};
}

double Beam::GetPbGlass() const
{
    return Processors::Beampolmon->PbGlass.Get() * 1.0e6 / Processors::Beampolmon->Reference_1MHz.Get();
}

double Beam::GetIonChamber() const
{
    return Processors::Beam->IonChamber.Get() * 1.0e6 / Processors::Beampolmon->Reference_1MHz.Get();
}

double Beam::GetFaradyCup() const
{
    return Processors::Beampolmon->FaradayCup.Get();
}
