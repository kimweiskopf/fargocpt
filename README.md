# Changes in the code
The code is changed by adding an additional $\beta$ for $\beta$-cooling , which results out of a first order taylor approximation of $Q_{surf}$. It is given by:


$\beta = t_{cool}  \Omega_{K}  \left ( 4 - \frac{2 T_{0}^{2} \times \left(\frac{3}{8} \tau_{0}  - \frac{4 \tau_{0}}{(4 \tau_0 \,T_0^{2}+\tau_{min})^{2}}\right)}{\tau_{eff}} \right)$

$\tau_{eff} = \frac{3}{8}\tau_{0} T_{0}^{2} + \frac{1}{2} + \frac{1}{4 \tau_{0} \, T_{0}^{2} +0.01}$

$\tau_{0} = \kappa_{0} \frac{\Sigma}{\sqrt{8 \pi}}$, $\kappa_{0} = 2 \cdot 10^{-4}$ $t_{cool} = \frac{e_{thermal}(T_0)}{Q_{cool}(T_0)}$

with the equilibrium temperature, from equating $Q_{irr} = Q_{surf}$:

$ T=\left( (1- \epsilon) \frac{L_{*}}{4 \pi \sigma_{SB}}  \frac{2}{7} \sqrt{\frac{\mathcal{R}}{\mu G M_{*}}}\, \right)^{2/7} r^{-3/7} $

There have been changes made in parameters.h, parameters.cpp, SourceEuler.cpp
