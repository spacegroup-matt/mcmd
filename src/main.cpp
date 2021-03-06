/*
 Douglas M. Franz
 University of South Florida
 Space group research
 2017
*/

// needed c/c++ libraries
#include <cmath>
#include <iostream>
#include <string>
#include <strings.h>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <fstream>
#include <stdio.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <map>
#include <vector>
#include <stdlib.h>
#include <time.h>
#include <chrono>
//#include <float.h>

// c++ code files of this program
// ORDER MATTERS HERE
#include "usefulmath.cpp"
#include "classes.cpp"
#include "system.cpp"
#include "fugacity.cpp"
#include "system_functions.cpp" 
#ifdef CUDA
    #include "cudafuncs.cu"  // CUDA STUFF
#endif
#include "mc.cpp" // this will include potential.cpp, which includes lj, coulombic, polar
#include "md.cpp"
#include "io.cpp"
#include "radial_dist.cpp"
#include "averages.cpp"
#include "histogram.cpp"


using namespace std;

int main(int argc, char **argv) {
    
	// start timing
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	double time_elapsed;
	double sec_per_step;

	srand((unsigned)time(NULL)); // initiate random seed

	// disable output buffering (print everything immediately to output)
	setbuf(stdout, NULL); // makes sure runlog output is fluid on SLURM etc.

    // SET UP THE SYSTEM
    System system;
	system.checkpoint("setting up system with main functions...");
    readInput(system, argv[1]); // executable takes the input file as only argument.
	readInAtoms(system, system.constants.atom_file);
	paramOverrideCheck(system);
	if (system.constants.autocenter)
        centerCoordinates(system);
    setupBox(system);
    if (system.stats.radial_dist)
        setupRadialDist(system);
    moleculePrintout(system); // this will confirm the sorbate to the user in the output. Also checks for system.constants.model_name and overrides the prototype sorbate accordingly.
    if (system.constants.histogram_option) {
        system.grids.histogram = (histogram_t *) calloc(1,sizeof(histogram_t));
        system.grids.avg_histogram = (histogram_t *) calloc(1,sizeof(histogram_t));
        setup_histogram(system);
        allocate_histogram_grid(system);
    }
    setupFugacity(system);
    initialize(system); // these are just system name sets,
    printf("SORBATE COUNT: %i\n", (int)system.proto.size());
    printf("VERSION NUMBER: %i\n", 336); // i.e. github commit
    system.checkpoint("Done with system setup functions.");

    // compute inital COM for all molecules, and moment of inertia
    // (io.cpp handles molecular masses //
    for (int i=0; i<system.molecules.size(); i++) {
        system.molecules[i].calc_center_of_mass();
        if (system.molecules[i].atoms.size() > 1) system.molecules[i].calc_inertia();
        for (int n=0;n<3;n++) system.molecules[i].original_com[n] = system.molecules[i].com[n]; // save original molecule COMs for diffusion calculation in MD.
    }

	// clobber files
	remove( system.constants.output_traj.c_str() ); remove( system.constants.thermo_output.c_str() );
	remove( system.constants.restart_pdb.c_str() ); remove ( system.constants.output_traj_pdb.c_str() );
	remove( system.stats.radial_file.c_str() ); remove( system.constants.output_histogram.c_str() );
	remove( system.constants.dipole_output.c_str() ); remove( system.constants.frozen_pdb.c_str() );
    remove( system.constants.restart_mov_pdb.c_str() );

    // INITIAL WRITEOUTS
    // Prep thermo output file
    FILE *f = fopen(system.constants.thermo_output.c_str(), "w");
    fprintf(f, "#step #TotalE(K) #LinKE(K)  #RotKE(K)  #PE(K) #RD(K) #ES(K) #POL(K) #density(g/mL) #temp(K) #pres(atm) #N\n");
    fclose(f);
    // Prep pdb trajectory if needed
    if (system.constants.pdb_traj_option) {
        if (system.constants.pdb_bigtraj_option) {
            FILE *f = fopen(system.constants.output_traj_pdb.c_str(), "w");
            fclose(f);
        } else {
            // also the movables traj (going to phase-out the old trajectory 
            // which writes frozen atoms every time
            FILE *g = fopen(system.constants.restart_mov_pdb.c_str(), "w");
            fclose(g);
        }
    }
		// Prep histogram if needed
		if (system.constants.histogram_option)
			system.file_pointers.fp_histogram = fopen(system.constants.output_histogram.c_str(), "w");
    // frozen .pdb (just the MOF, usually)
    if (system.stats.count_frozens > 0) {
        writePDBfrozens(system, system.constants.frozen_pdb.c_str());
    }
    // END INTIAL WRITEOUTS

    system.checkpoint("Initial protocols complete. Starting MC or MD.");

    // BEGIN MC OR MD ===========================================================
	// =========================== MONTE CARLO ===================================
	if (system.constants.mode == "mc") {
	printf("\n| ================================== |\n");
	printf("|  BEGINNING MONTE CARLO SIMULATION  |\n");
	printf("| ================================== |\n\n");

    //outputCorrtime(system, 0); // do initial output before starting mc

    int frame = 1;
    int stepsize = system.constants.stepsize;
    int finalstep = system.constants.finalstep;
    int corrtime = system.constants.mc_corrtime; // print output every corrtime steps

    // RESIZE A MATRIX IF POLAR IS ACTIVE (and initialize the dipole file)
    if (system.constants.potential_form == POTENTIAL_LJESPOLAR || system.constants.potential_form == POTENTIAL_LJPOLAR || system.constants.potential_form == POTENTIAL_COMMYESPOLAR) {
				FILE * fp = fopen(system.constants.dipole_output.c_str(), "w");
				fclose(fp);

        system.last.total_atoms = system.constants.total_atoms;
        int N = 3 * system.constants.total_atoms;
        system.constants.A_matrix= (double **) calloc(N,sizeof(double*));
        for (int i=0; i< N; i++ ) {
            system.constants.A_matrix[i]= (double *) malloc(N*sizeof(double));
        }

        system.last.thole_total_atoms = system.constants.total_atoms;

        makeAtomMap(system); // writes the atom indices
    }

    // begin timing for steps "begin_steps"
	std::chrono::steady_clock::time_point begin_steps = std::chrono::steady_clock::now();

	// MAIN MC STEP LOOP
	int corrtime_iter=1;
	for (int t=0; t <= (finalstep-system.constants.step_offset); t+=stepsize) { // 0 is initial step
		system.checkpoint("New MC step starting."); //printf("Step %i\n",t);
        system.stats.MCstep = t;
        system.stats.MCcorrtime_iter = corrtime_iter;

				// DO MC STEP
                if (t!=0) {
                    setCheckpoint(system); // save all the relevant values in case we need to revert something.
                    //make_pairs(system); // establish pair quantities
                    //computeDistances(system);
                    runMonteCarloStep(system);
                    system.checkpoint("...finished runMonteCarloStep");

                    if (system.stats.MCmoveAccepted == false)
                        revertToCheckpoint(system);
                    else if (system.constants.simulated_annealing) { // S.A. only goes when move is accepted.
                        system.constants.temp =
                            system.constants.sa_target +
                            (system.constants.temp - system.constants.sa_target) *
                            system.constants.sa_schedule;
                    }

                    //computeAverages(system);
                } else {
                    computeInitialValues(system);
                }

        // CHECK FOR CORRTIME
        if (t==0 || t % corrtime == 0 || t == finalstep) { /// output every x steps

			// get all observable averages
            computeAverages(system);

			// prep histogram for writing.
			if (system.constants.histogram_option) {
			    zero_grid(system.grids.histogram->grid,system);
                population_histogram(system);
                if (t != 0) update_root_histogram(system);
            }
            /* -------------------------------- */
			// [[[[ PRINT OUTPUT VALUES ]]]]
			/* -------------------------------- */

            // TIMING
            std::chrono::steady_clock::time_point end= std::chrono::steady_clock::now();
            time_elapsed = (std::chrono::duration_cast<std::chrono::microseconds>(end - begin_steps).count()) /1000000.0;
            sec_per_step = time_elapsed/t;
            double progress = (((float)t)/finalstep*100);
            double ETA = ((time_elapsed*finalstep/t) - time_elapsed)/60.0;
            double ETA_hrs = ETA/60.0;
            double efficiency = system.stats.MCeffRsq / time_elapsed;

			// PRINT MAIN OUTPUT
			printf("MONTE CARLO\n");
            printf("%s %s\n",system.constants.jobname.c_str(),argv[1]);
            if (!system.constants.simulated_annealing)
			    printf("ENSEMBLE: %s; T = %.3f K\n", system.constants.ensemble_str.c_str(), system.constants.temp);
			else
                printf("ENSEMBLE: %s; T = %.3f K (Simulated annealing on)\n",system.constants.ensemble_str.c_str(), system.constants.temp);

            printf("Input atoms: %s\n",system.constants.atom_file.c_str());
			printf("Step: %i / %i; Progress = %.3f%%; Efficiency = %.3f\n",system.stats.MCstep+system.constants.step_offset,finalstep,progress,efficiency);
			printf("Time elapsed = %.2f s = %.4f sec/step; ETA = %.3f min = %.3f hrs\n",time_elapsed,sec_per_step,ETA,ETA_hrs);

            printf("BF avg = %.4f       ( %.3f Ins / %.3f Rem / %.3f Dis / %.3f Vol ) \n",
				system.stats.bf_avg,
				system.stats.ibf_avg,
				system.stats.rbf_avg,
				system.stats.dbf_avg,
				system.stats.vbf_avg);

			printf("AR avg = %.4f       ( %.3f Ins / %.3f Rem / %.3f Dis / %.3f Vol )  \n",
				(system.stats.ar_tot),
				(system.stats.ar_ins),
				(system.stats.ar_rem),
				(system.stats.ar_dis),
				(system.stats.ar_vol));
			printf("Total accepts: %i ( %.2f%% Ins / %.2f%% Rem / %.2f%% Dis / %.2f%% Vol )  \n",
				(int)system.stats.total_accepts,
			    system.stats.ins_perc,
                system.stats.rem_perc,
                system.stats.dis_perc,
                system.stats.vol_perc);
            printf("RD avg =              %.5f +- %.5f K\n", // (LJ = %.4f, LRC = %.4f, LRC_self = %.4f)\n",
                system.stats.rd.average, system.stats.rd.sd); //, system.stats.lj.average, system.stats.lj_lrc.average, system.stats.lj_self_lrc.average);
			printf("ES avg =              %.5f +- %.5f K\n", //(real = %.4f, recip = %.4f, self = %.4f)\n",
                system.stats.es.average, system.stats.es.sd); //, system.stats.es_real.average, system.stats.es_recip.average, system.stats.es_self.average);
			printf("Polar avg =           %.5f +- %.5f K\n",system.stats.polar.average, system.stats.polar.sd);
			printf("Total potential avg = %.5f +- %.5f K\n",system.stats.potential.average, system.stats.potential.sd);
			printf("Volume avg  = %.2f +- %.2f A^3 = %.2f nm^3\n",system.stats.volume.average, system.stats.volume.sd, system.stats.volume.average/1000.0);
			for (int i=0; i<system.proto.size(); i++) {
                double mmolg = system.stats.wtpME[i].average * 10 / (system.proto[i].mass*1000*system.constants.NA);
                double cm3gSTP = mmolg*22.4;
                double mgg = mmolg * (system.proto[i].mass*1000*system.constants.NA);
                if (system.stats.count_frozens > 0) {
                    printf("-> %s wt %%    = %.5f +- %.5f %%; %.5f cm^3/g (STP)\n", system.proto[i].name.c_str(), system.stats.wtp[i].average, system.stats.wtp[i].sd, cm3gSTP);
                    printf("      wt %% ME = %.5f +- %.5f %%; %.5f mmol/g\n",system.stats.wtpME[i].average, system.stats.wtpME[i].sd, mmolg);
                }
                printf("      N_movables avg = %.3f +- %.3f; %.5f mg/g\n",
                system.stats.Nmov[i].average, system.stats.Nmov[i].sd, mgg);
                if (system.stats.excess[i].average > 0 || system.constants.free_volume >0)
                    printf("      Excess adsorption ratio = %.5f +- %.5f mg/g\n", system.stats.excess[i].average, system.stats.excess[i].sd);
                printf("      Density avg = %.6f +- %.3f g/mL = %6f g/L \n",system.stats.density[i].average, system.stats.density[i].sd, system.stats.density[i].average*1000.0);
                if (system.proto.size() > 1)
                    printf("      Selectivity = %.3f +- %.3f\n",system.stats.selectivity[i].average, system.stats.selectivity[i].sd);
            }
            if (system.constants.ensemble == ENSEMBLE_UVT) {
                if (system.proto.size() == 1) {
                    if (system.stats.qst.average > 0)
                        printf("Qst = %.5f kJ/mol\n", system.stats.qst.value); //, system.stats.qst.sd);
                    if (system.stats.qst_nvt.average > 0)
                        printf("U/N avg = %.5f kJ/mol\n", system.stats.qst_nvt.value); //, system.stats.qst_nvt.sd);
                }
                printf("N_molecules = %i; N_movables = %i; N_sites = %i\n", (int)system.molecules.size(), system.stats.count_movables, system.constants.total_atoms);
            }
            /*
            if (system.constants.ensemble != "npt") {
                printf("Chemical potential avg = %.4f +- %.4f K / sorbate molecule \n", system.stats.chempot.average, system.stats.chempot.sd);
            }
            */
            if (system.proto.size() == 1 && system.stats.count_frozen_molecules == 0)
                printf("Compressibility factor Z avg = %.6f +- %.6f (for homogeneous gas %s) \n",system.stats.z.average, system.stats.z.sd, system.proto[0].name.c_str());
            if (system.constants.dist_within_option) {
                printf("N of %s within %.5f A of origin: %.5f +- %.3f (actual: %i)\n", system.constants.dist_within_target.c_str(), system.constants.dist_within_radius, system.stats.dist_within.average, system.stats.dist_within.sd, (int)system.stats.dist_within.value);
            }

            printf("--------------------\n\n");

            // CONSOLIDATE ATOM AND MOLECULE PDBID's
            // quick loop through all atoms to make PDBID's pretty (1->N)
            if (system.molecules.size() > 0) {
            int molec_counter=1, atom_counter=1;
            for (int i=0; i<system.molecules.size(); i++) {
                system.molecules[i].PDBID = molec_counter;
                for (int j=0; j<system.molecules[i].atoms.size(); j++) {
                    system.molecules[i].atoms[j].PDBID = atom_counter;
                    system.molecules[i].atoms[j].mol_PDBID = system.molecules[i].PDBID;
                    atom_counter++;
                } // end loop j
                molec_counter++;
            } // end loop i
            // WRITE RESTART FILE AND OTHER OUTPUTS
            if (system.constants.xyz_traj_option)
                writeXYZ(system,system.constants.output_traj,frame,t,0,system.constants.xyz_traj_movers_option);
            frame++;
            writePDB(system, system.constants.restart_pdb); // all atoms
            if (!system.constants.pdb_bigtraj_option) writePDBmovables(system, system.constants.restart_mov_pdb); // only movers
            if (system.constants.pdb_traj_option) {
                if (system.constants.pdb_bigtraj_option)
                    writePDBtraj(system, system.constants.restart_pdb, system.constants.output_traj_pdb, t); // all atoms
                else writePDBtraj(system, system.constants.restart_mov_pdb, system.constants.output_traj_movers_pdb,t); // just movers
            }
            // ONLY WRITES DENSITY FOR FIRST SORBATE
            writeThermo(system, system.stats.potential.value, 0.0, 0.0, system.stats.potential.value, system.stats.rd.value, system.stats.es.value, system.stats.polar.value, system.stats.density[0].value*1000, system.constants.temp, system.constants.pres, t, system.stats.Nmov[0].value);
            if (system.stats.radial_dist) {
                radialDist(system);
                writeRadialDist(system);
            }
						if (t != 0 && system.constants.histogram_option)
							write_histogram(system.file_pointers.fp_histogram, system.grids.avg_histogram->grid, system);

						if ((system.constants.potential_form == POTENTIAL_LJPOLAR || system.constants.potential_form == POTENTIAL_LJESPOLAR) && system.constants.dipole_output_option)
							write_dipole(system);
            } // end if N > 0
            // count the corrtime occurences.
            corrtime_iter++;

		} // END IF CORRTIME
	} // END MC STEPS LOOP.

	// FINAL EXIT OUTPUT
    if (system.constants.ensemble == ENSEMBLE_NPT) {
	    printf("Final basis parameters: \n");
        system.pbc.printBasis();
    }
	printf("Insert accepts:        %i\n", system.stats.insert_accepts);
	printf("Remove accepts:        %i\n", system.stats.remove_accepts);
	printf("Displace accepts:      %i\n", system.stats.displace_accepts);
	printf("Volume change accepts: %i\n", system.stats.volume_change_accepts);
    printf("Auto-rejects (r <= %.5f A): %i\n", system.constants.auto_reject_r, system.constants.rejects);

	std::chrono::steady_clock::time_point end= std::chrono::steady_clock::now();
        time_elapsed = (std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()) /1000000.0;
	printf("Total wall time = %f s\n",time_elapsed);

    if (system.constants.potential_form == POTENTIAL_LJESPOLAR || system.constants.potential_form == POTENTIAL_LJPOLAR) {
        printf("Freeing data structures... ");
        for (int i=0; i< 3* system.constants.total_atoms; i++) {
            free(system.constants.A_matrix[i]);
        }
        free(system.constants.A_matrix); system.constants.A_matrix = NULL;
    }
    printf("done.\n");
	printf("MC steps completed. Exiting program.\n"); std::exit(0);
	}
	// ===================== END MONTE CARLO ================================================



	// ===================== MOLECULAR DYNAMICS ==============================================
	else if (system.constants.mode == "md") {
        double v_component=0., v_init=0.;

        // write initial XYZ
        if (system.constants.xyz_traj_option)
            writeXYZ(system,system.constants.output_traj, 1, 0, 0, system.constants.xyz_traj_movers_option);
        int frame = 2; // weird way to initialize but it works for the output file.
        // and initial PDB
        writePDB(system,system.constants.restart_pdb);
            if (system.constants.pdb_traj_option && system.constants.pdb_bigtraj_option)
                writePDBtraj(system,system.constants.restart_pdb, system.constants.output_traj_pdb, 0);

	// assign initial velocities
    double randv; double DEFAULT = 99999.99;
    if (system.constants.md_init_vel != DEFAULT) {
        // if user defined
        for (int i=0; i<system.molecules.size(); i++) {
            for (int n=0; n<3; n++) {
                randv = ((double)rand() / (double)RAND_MAX *2 - 1) * system.constants.md_init_vel;
                system.molecules[i].vel[n] = randv; // that is, +- 0->1 * user param
            }
        }
        printf("Computed initial velocities via user-defined value: %f A/fs\n",system.constants.md_init_vel);
				system.constants.md_vel_goal = sqrt(system.constants.md_init_vel*system.constants.md_init_vel/3.0);
    } else {
        // otherwise use temperature as default via v_RMS
        // default temp is zero so init. vel's will be 0 if no temp is given.

        //v_init = 1e-5 * sqrt(8.0*system.constants.temp*system.constants.kb/system.proto[0].mass/M_PI);
				//v_component = sqrt(v_init*v_init / 3.0);

//THIS IS WORKING BEFORE BUT SEEMED TO HAVE HIGH INITIAL v
// when i swap to the above, the equilibrium T seems much lower.
				// Frenkel method for NVT v_alpha determination (converted to A/fs) p140
				v_component = 1e-5 * sqrt(system.constants.kb*system.constants.temp/system.proto[0].mass);
        v_init = sqrt(3*v_component*v_component);


// GARBAGE
//double v_init = sqrt(8.0 * system.constants.R * system.constants.temp /
  //          (M_PI*system.proto[0].mass*system.constants.NA)) / 1e5; // THIS IS NOT GOOD FOR MULTISORBATE SYSTEM YET. A/fs
    //    double v_component = sqrt(v_init*v_init / 3.0);

        double pm = 0;
        for (int i=0; i<system.molecules.size(); i++) {
            for (int n=0; n<3; n++) {
                randv = (double)rand() / (double)RAND_MAX;
                if (randv > 0.5) pm = 1.0;
                else pm = -1.0;

                system.molecules[i].vel[n] = v_component * pm;
            }
        }

        printf("Computed initial velocities from temperature: v_init = %f A/fs\n",v_init);
        system.constants.md_init_vel = v_init;
    }
    system.constants.md_vel_goal = v_component; //sqrt(system.constants.md_init_vel*system.constants.md_init_vel/3.0);
   /*
        printf("md_vel_goal = %f; v_component = %f; md_init_vel = %f; v_init = %f\n",
        system.constants.md_vel_goal,
        v_component,
        system.constants.md_init_vel,
        v_init);
    */
    // end initial velocities

	double dt = system.constants.md_dt; // * 1e-15; //0.1e-15; // 1e-15 is one femptosecond.
	double tf = system.constants.md_ft; // * 1e-15; //100e-15; // 100,000e-15 would be 1e-9 seconds, or 1 nanosecond.
	int total_steps = floor(tf/dt);
	int count_md_steps = 1;
    double diffusion_d[3] = {0,0,0}, diffusion_sum=0., D=0.0;
	double KE=0., PE=0., TE=0., Temp=0., v_avg=0., Klin=0., Krot=0., pressure=0.; //, Ek=0.;
    int i,n;
        printf("\n| ========================================= |\n");
        printf("|  BEGINNING MOLECULAR DYNAMICS SIMULATION  |\n");
        printf("| ========================================= |\n\n");

	// begin timing for steps
	std::chrono::steady_clock::time_point begin_steps = std::chrono::steady_clock::now();
	for (double t=dt; t <= tf; t=t+dt) {
        system.stats.MDtime = t;
		//printf("%f\n",t);
		integrate(system,dt);

		if (count_md_steps % system.constants.md_corrtime == 0 || t==dt || t==tf) {  // print every x steps and first and last.
            if (system.constants.histogram_option) {
							zero_grid(system.grids.histogram->grid,system);
                            population_histogram(system);
                            if (t != dt) update_root_histogram(system);
            }

            // get KE and PE and T at this step.
            double* ETarray = calculateEnergyAndTemp(system, t);
            KE = ETarray[0] * system.constants.K2KJMOL;
            PE = ETarray[1] * system.constants.K2KJMOL;
            TE = KE+PE;
            Temp = ETarray[2];
            v_avg = ETarray[3];
            //Ek = ETarray[4]; // Equipartition Kinetic energy (apparently). Not even using.
            Klin = ETarray[5] * system.constants.K2KJMOL;
            Krot = ETarray[6] * system.constants.K2KJMOL;
            pressure = ETarray[7]; // only good for NVT. Frenkel p84
            system.stats.csp.value = (TE*1000/system.constants.NA);
                system.stats.csp.value /= -((Temp)*system.proto[0].mass*1000*system.stats.count_movables);
                system.stats.csp.calcNewStats(); // the minus above i think is needed.
            system.stats.temperature.value = Temp;
                system.stats.temperature.calcNewStats();

            // calc diffusion
            // R^2 as a function of time should be linear.
            diffusion_sum=0.;
            for (i=0; i<system.molecules.size(); i++) {
                for (n=0; n<3; n++)
                    diffusion_d[n] = system.molecules[i].com[n] + system.molecules[i].diffusion_corr[n] - system.molecules[i].original_com[n];

                diffusion_sum += dddotprod(diffusion_d, diffusion_d); // the net R^2 from start -> now
            }
            D = (diffusion_sum / system.stats.count_movables)/(6.0*t); // 6 because 2*dimensionality = 2*3
            D *= 0.1; // A^2 per fs -> cm^2 per sec (CGS units).
            system.stats.diffusion.value = D;
            system.stats.diffusion.calcNewStats();

            // PRESSURE (my pathetic nRT/V method)
						// using this until i get a decent NVT frenkel method working.
						// PE since it's I.G. approximation.
						double nmol = (system.proto[0].mass*system.stats.count_movables)/(system.proto[0].mass*system.constants.NA);
						system.stats.pressure.value = nmol*system.constants.R*system.stats.temperature.value/(system.pbc.volume*system.constants.A32L) * system.constants.JL2ATM;
						system.stats.pressure.calcNewStats();
						//pressure = -PE/system.constants.volume * system.constants.kb * 1e30 * 9.86923e-6; // P/V to atm

			// PRINT OUTPUT
			std::chrono::steady_clock::time_point end= std::chrono::steady_clock::now();
			time_elapsed = (std::chrono::duration_cast<std::chrono::microseconds>(end - begin_steps).count()) /1000000.0;
			sec_per_step = time_elapsed/(double)count_md_steps;
			double progress = (((float)count_md_steps)/(float)total_steps*100);
			double ETA = ((time_elapsed*(float)total_steps/(float)count_md_steps) - time_elapsed)/60.0;
			double ETA_hrs = ETA/60.0;
            double outputTime; string timeunit;
            if (t > 1e9) {
                outputTime = t/1e9; timeunit="us";
            } else if (t > 1e6) {
                outputTime = t/1e6; timeunit="ns";
            } else if (t > 1e3) {
                outputTime = t/1e3; timeunit="ps";
            } else {
                outputTime = t; timeunit="fs";
            }

            printf("MOLECULAR DYNAMICS\n");
            //printf("testing angular velocity\n");
            printf("%s %s\n",system.constants.jobname.c_str(),argv[1]);
            printf("ENSEMBLE: %s; N_molecules = %i; N_atoms = %i\n",system.constants.ensemble_str.c_str(), system.stats.count_movables, system.constants.total_atoms);
            printf("Input atoms: %s\n",system.constants.atom_file.c_str());
            printf("Step: %i / %i; Progress = %.3f%%; Realtime = %.5f %s\n",count_md_steps,total_steps,progress,outputTime, timeunit.c_str());
            printf("Time elapsed = %.2f s = %.4f sec/step; ETA = %.3f min = %.3f hrs\n",time_elapsed,sec_per_step,ETA,ETA_hrs);
			printf("Input T: %.3f K; Emergent T avg = %.4f +- %.4f K; Current = %.4f K\n",system.constants.temp, system.stats.temperature.average, system.stats.temperature.sd, Temp);
            printf("KE: %.3f kJ/mol (lin: %.3f , rot: %.3e )\n",
                  KE, Klin, Krot );
            printf("PE: %.3f kJ/mol; Total E: %.3f kJ/mol; \n",
                  PE, TE
                  );
            printf("Average v = %.5f A/fs; v_init = %.5f A/fs\nEmergent Pressure avg: %.3f +- %.3f atm (I.G. approx)\n",
                v_avg, system.constants.md_init_vel, system.stats.pressure.average, system.stats.pressure.sd );
            // hiding specific heat until i make sure it's right.
            //printf("Specific heat: %.4f +- %.4f J/gK\n", system.stats.csp.average, system.stats.csp.sd );
            if (system.constants.md_pbc) {
                printf("Diffusion coefficient = %.4e cm^2 / s (%s homogenous)\n", system.stats.diffusion.value, system.proto[0].name.c_str());
			}
            //printf("   --> instantaneous D = %.4e cm^2 / s\n", system.stats.diffusion.value);
            printf("--------------------\n\n");

            // WRITE OUTPUT FILES
            if (system.constants.xyz_traj_option)
			    writeXYZ(system,system.constants.output_traj,frame,count_md_steps,t, system.constants.xyz_traj_movers_option);
            frame++;
            writeThermo(system, TE, Klin, Krot, PE, system.stats.rd.value, system.stats.es.value, system.stats.polar.value, 0.0, system.stats.temperature.average, pressure, count_md_steps, system.stats.Nmov[0].value);
            // restart file.
            writePDB(system, system.constants.restart_pdb); // containing all atoms

            // trajectory file
            if (!system.constants.pdb_bigtraj_option) writePDBmovables(system, system.constants.restart_mov_pdb); // only movers restart frame
            if (system.constants.pdb_traj_option) {
                if (system.constants.pdb_bigtraj_option)
                    writePDBtraj(system, system.constants.restart_pdb, system.constants.output_traj_pdb, t); // copy all-atoms-restart-PDB to PDB trajectory
                else writePDBtraj(system, system.constants.restart_mov_pdb, system.constants.output_traj_movers_pdb,t); // just movers to PDB trajectory
            }
            if (system.stats.radial_dist) {
                radialDist(system);
                writeRadialDist(system);
            }
            if (t != dt && system.constants.histogram_option)
				write_histogram(system.file_pointers.fp_histogram, system.grids.avg_histogram->grid, system);
		}
		count_md_steps++;
	} // end MD timestep loop
	}
}
