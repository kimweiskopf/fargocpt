"""
Test for Super-Time stepping from D'Angelo et al. 2003 THERMOHYDRODYNAMICS OF CIRCUMSTELLAR DISKS WITH HIGH-MASS PLANETS
"""
import subprocess
import os
import argparse

import numpy as np
import matplotlib.pyplot as plt

from scipy import interpolate, integrate

quants = ["vrad", "Sigma", "Temperature", "energy"]
quants_key = {"vrad": 0, "Sigma": 1, "Temperature": 2, "energy": 3}

acceptable_diff = {
    "vrad" : 0.0153, 
    "Sigma" : 0.0073,
    "Temperature" : 0.016, 
    "energy" : 0.014
}

test_cases = {
    "SN" : {
        "outdir" : '../../output/tests/shocktube/SN/',
        "color" : "red",
        "ls" : "--"
    },
    "TW" : {
        "outdir" : '../../output/tests/shocktube/TW/',
        "color" : "blue",
        "ls" : "--"
    },
    "TW LF" : {
        "outdir" : '../../output/tests/shocktube/TW_LF/',
        "color" : "green",
        "ls" : "-."
    },
    "SN LF" : {
        "outdir" : '../../output/tests/shocktube/SN_LF/',
        "color" : "orange",
        "ls" : ":"
    }
}


def get_analytic_spl(quant):
    analytic_data = np.loadtxt("analytic_shock.dat", skiprows=2)
    x = analytic_data[:,1]
    i = quants_key[quant]

    if i is None:
        raise ValueError(f"{quant} is not a valid quantity")
    y = analytic_data[:,(i+2)]
    if quant == 'energy':
      y = analytic_data[:,4]*analytic_data[:,3]/(1.4-1)
      
    s = interpolate.InterpolatedUnivariateSpline(x, y)

    return s

def analytic(axs):
    analytic_data = np.loadtxt("analytic_shock.dat", skiprows=2)
    x = analytic_data[:,1]

    for ind in range(len(quants)):
        quant = quants[ind]
        i = quants_key[quant]
        ax = axs[ind]
        y = analytic_data[:,(i+2)]
        if quant == 'energy':
            y = analytic_data[:,4]*analytic_data[:,3]/(1.4-1)

        ax.plot(x, y, '-k', label='Analytic', lw=2)


def plot_output(out, label, color, Nsnap, axs, ls="--"):

    r12 = np.loadtxt(out + "used_rad.dat", skiprows=0)
    r1 = 0.5*(r12[1:] + r12[:-1])-r12[0]
    file_name = f"{out}/snapshots/{Nsnap}/Sigma.dat"
    data = np.fromfile(file_name)
    N = len(data)
    nr = len(r1)
    nphi = int(N/nr)

    for ind in range(len(quants)):
        quant = quants[ind]
        i = quants_key[quant]
        ax = axs[ind]

        name = quant

        file_name = f"{out}/snapshots/{Nsnap}/{name}.dat"
        data = np.fromfile(file_name)

        if name == 'vrad':
            data = data.reshape((nr+1, nphi))
            data = np.mean(data, axis=1)
            data = 0.5*(data[1:] + data[:-1])
        else:
            data = data.reshape((nr, nphi))
            data = np.mean(data, axis=1)


        ax.plot(r1, data, ls=ls, color=color, label=label, lw=2.5)


def diff_to_analytic(out, quant, Nsnap):

    r12 = np.loadtxt(out + "used_rad.dat", skiprows=0)
    r1 = 0.5*(r12[1:] + r12[:-1])-r12[0]
    file_name = f"{out}/snapshots/{Nsnap}/Sigma.dat"
    data = np.fromfile(file_name)
    N = len(data)
    nr = len(r1)
    nphi = int(N/nr)


    name = quant
    file_name = f"{out}/snapshots/{Nsnap}/{name}.dat"
    data = np.fromfile(file_name)
    if name == 'vrad':
        data = data.reshape((nr+1, nphi))
        data = np.mean(data, 1)
        data = 0.5*(data[1:] + data[:-1])
    else:
        data = data.reshape((nr, nphi))
        data = np.mean(data, 1)
    # restrict to 0 to 1
    inds = np.logical_and(r1 >= 0, r1 <=1)
    r1 = r1[inds]
    data = data[inds] 

    spl = get_analytic_spl(quant)
    analytic = spl(r1)

    delta = np.abs(data - analytic)
    return integrate.simpson(delta, x=r1)


def visualize(Nsnapshot):
    fig, axs = plt.subplots(2,2,figsize=(6,8))
    axs = np.ravel(axs)

    analytic(axs)
    for key, val in test_cases.items():
        if not os.path.exists(val["outdir"]):
            continue

        plot_output(val["outdir"], 
                    key, 
                    val["color"], 
                    Nsnapshot, 
                    axs, 
                    ls=val["ls"])

    for ind in range(len(quants)):
        ax = axs[ind]
        quant = quants[ind]
        i = quants_key[quant]
        ax.axis('auto')
        ax.set_title(quant, color='black', y = 0.99)
        if quant == 'Sigma':
            ax.legend(loc='upper right')
        if quant == 'energy':
            ax.legend(loc='upper right')
        if quant == 'vrad':
            ax.legend(loc='upper left')
        if quant == 'Temperature':
            ax.legend(loc='lower left')

    # plt.savefig('ShockTube.pdf', dpi=300, bbox_inches='tight')
    # plt.show()
    fig.savefig('plot.jpg', dpi=150, bbox_inches='tight')


def test(_):
    
    success = True
    with open("diffs.log", "w") as logfile:
        from datetime import datetime
        current_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"{current_time}", file=logfile)

        for key, val in test_cases.items():
            outdir = val["outdir"]
            if not os.path.exists(outdir):
                continue
            for quant in quants:
                diff = diff_to_analytic(outdir, quant, 1)
                is_smaller = diff < acceptable_diff[quant]
                if not is_smaller:
                    success = False
                print("SUCCESS" if is_smaller else "FAIL", "|",
                      key, "|", quant, "|", diff, file=logfile)
    if success:
        print("SUCCESS: shocktube test")
    else:
        print("FAIL: shocktube test, threshold of integrated diff exceeded")
    
    visualize(1)


if __name__=="__main__":
    test("dummy")
