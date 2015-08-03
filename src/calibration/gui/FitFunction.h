#pragma once

#include "base/interval.h"
#include "calibration/gui/GUIbase.h"

#include <memory>

#include "TF1Knobs.h"

#include <list>
#include <string>
#include "base/std_ext.h"
#include "base/interval.h"

#include "Rtypes.h"

class TH1;
class TF1;

namespace ant {
namespace calibration {
namespace gui {


class FitFunction {
public:
    using knoblist_t = std::list<std::unique_ptr<VirtualKnob>>;
    using SavedState_t = std::vector<double>;

protected:
    knoblist_t knobs;

    template <typename T, typename ... Args_t>
    void Addknob(Args_t&& ... args) {
        knobs.emplace_back(std_ext::make_unique<T>(std::forward<Args_t>(args)...));
    }

    static ant::interval<double> getRange(const TF1* func);
    static void setRange(TF1* func, const ant::interval<double>& i);

    static void saveTF1(const TF1* func, SavedState_t& out);
    static void loadTF1(SavedState_t::const_iterator& data_pos, TF1* func);

public:
    virtual ~FitFunction();
    virtual void Draw() =0;
    knoblist_t& getKnobs() { return knobs; }
    virtual void Fit(TH1* hist) =0;

    /**
     * @brief Set/Calcualte default parameter values. The hist that will be fitted later is given to allow adaptions
     * @param hist The hist to fit later
     */
    virtual void SetDefaults(TH1* hist) =0;

    virtual void SetRange(ant::interval<double> i) =0;
    virtual ant::interval<double> GetRange() const =0;
    virtual void Sync() {}
    virtual void SetPoints(int n) =0;

    virtual SavedState_t Save() const =0;
    virtual void Load(const std::vector<double>& data) =0;

};



class FitFunctionGaus: public FitFunction {
protected:
    TF1* func = nullptr;

    class MyWKnob: public VirtualKnob {
    protected:
        TF1* func = nullptr;
    public:

        MyWKnob(const std::string& n, TF1* Func);

        virtual double get() const override;
        virtual void set(double a) override;

    };

public:
    FitFunctionGaus();

    virtual ~FitFunctionGaus();

    virtual void Draw() override;

    virtual void Fit(TH1* hist) override;
    virtual void SetDefaults(TH1* hist) override;

    virtual void SetRange(ant::interval<double> i) override;
    virtual ant::interval<double> GetRange() const override;
    virtual void SetPoints(int n) override;

    virtual SavedState_t Save() const override;
    virtual void Load(const SavedState_t &data) override;
};

}
}
}

