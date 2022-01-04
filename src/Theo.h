#ifndef THEO_H
#define THEO_H

#include "constants.h"
#include "global.h"
#include "types.h"
#include <math.h>

double calculate_omega_kepler(double r);
void RefillSigma(t_polargrid *Density);
void RefillEnergy(t_polargrid *Energy);
double eggleton_1983(const double q, const double r);

#endif // THEO_H
