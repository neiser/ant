#pragma once

class TH1;
class TH2;

namespace ant {
class Omega {

public:

    struct FitResult {
        double pos;
        double N;
        double width;
        double chi2dof;
        FitResult(double pos_, double N_, double width_, double chi2dof_):
            pos(pos_),
            N(N_),
            width(width_),
            chi2dof(chi2dof_) {}
    };

    static FitResult FitHist(TH1* h, const bool fixOmegaMass=false, const double r_min=650.0, const double r_max=900.0);

    static void FitOmegaPeak(const bool fixOmegaMass=false, const double r_min=650.0, const double r_max=900.0);

    static void FitOmega2D(TH2* h);
};

}