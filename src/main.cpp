/*
 Douglas M. Franz
 University of South Florida
 Space group research
 2017
*/

// needed c/c++ libraries
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
#include <string>
#include <vector>
#include <stdlib.h>
#include <time.h>
#include <chrono>
//#include <float.h>

// c++ code files of this program
// ORDER MATTERS HERE
#include <usefulmath.cpp>
#include <constants.cpp>
#include <system.cpp>
#include <system_functions.cpp>
#include <mc.cpp> // this will include potential.cpp, which includes lj, coulombic, polar
#include <md.cpp>
#include <io.cpp>
#include <radial_dist.cpp>
#include <averages.cpp>

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
	readInput(system, argv[1]); // executable takes the input file as only argument.
	readInAtoms(system, system.constants.atom_file);
	paramOverrideCheck(system);	
	if (system.constants.autocenter == "on")
        centerCoordinates(system);
    setupBox(system);
    if (system.stats.radial_dist == "on")   
        setupRadialDist(system);
    moleculePrintout(system); // this will confirm the sorbate to the user in the output. Also checks for system.constants.model_name and overrides the prototype sorbate accordingly. 
    initialize(system); // these are just system name sets, 
    printf("VERSION NUMBER: %s\n", "137");


    // compute inital COM for all molecules, and moment of inertia
    // (io.cpp handles molecular masses // 
    for (int i=0; i<system.molecules.size(); i++) {
        system.molecules[i].calc_center_of_mass();
        if (system.molecules[i].atoms.size() > 1) system.molecules[i].calc_inertia();
    }

	// clobber files 
	remove( system.constants.output_traj.c_str() ); remove( system.constants.thermo_output.c_str() );
	remove( system.constants.restart_pdb.c_str() ); remove ( system.constants.output_traj_pdb.c_str() );
	remove( system.stats.radial_file.c_str() );

    // INITIAL WRITEOUTS
    // Prep thermo output file
    FILE *f = fopen(system.constants.thermo_output.c_str(), "w");
    fprintf(f, "#step #TotalE(K) #LinKE(K)  #RotKE(K)  #PE(K)  #density(g/mL) #temp(K) #pres(atm)\n");
    fclose(f);

    // Prep pdb trajectory if needed
    if (system.constants.pdb_traj_option == "on") {
        FILE *f = fopen(system.constants.output_traj_pdb.c_str(), "w");
        fclose(f); 
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

    // RESIZE A MATRIX IF POLAR IS ACTIVE
    if (system.constants.potential_form == "ljespolar") {
        system.constants.old_total_atoms = system.constants.total_atoms;
        int N = 3 * system.constants.total_atoms;
        system.constants.A_matrix= (double **) calloc(N,sizeof(double*));
        for (int i=0; i< N; i++ ) {
            system.constants.A_matrix[i]= (double *) malloc(N*sizeof(double));
        }
    }

    // begin timing for steps "begin_steps"
	std::chrono::steady_clock::time_point begin_steps = std::chrono::steady_clock::now();	
	
	// MAIN MC STEP LOOP
	int corrtime_iter=1;
	for (int t=0; t <= finalstep; t+=stepsize) { // 0 is initial step
		system.checkpoint("New MC step starting."); //printf("Step %i\n",t);	
        system.stats.MCstep = t;
        system.stats.MCcorrtime_iter = corrtime_iter;

				// DO MC STEP
                if (t!=0) {
                    setCheckpoint(system); // save all the relevant values in case we need to revert something.
				    runMonteCarloStep(system,system.constants.potential_form);
                    system.checkpoint("...finished runMonteCarloStep");
                    system.constants.old_total_atoms = system.constants.total_atoms;
                    
                    if (system.stats.MCmoveAccepted == false)
                        revertToCheckpoint(system);

                    //computeAverages(system);
                } else {
                    computeInitialValues(system);
                }

        // CHECK FOR CORRTIME
        if (t==0 || t % corrtime == 0 || t == finalstep) { /// write every x steps

            computeAverages(system);

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
			printf("ENSEMBLE: %s; T = %.3f K; P = %.3f atm\n",system.constants.ensemble.c_str(), system.constants.temp, system.constants.pres);
			printf("Input atoms: %s\n",system.constants.atom_file.c_str());
			printf("Step: %i / %i; Progress = %.3f%%; Efficiency = %.3f\n",system.stats.MCstep,finalstep,progress,efficiency);
			printf("Time elapsed = %.2f s = %.3f sec/step; ETA = %.3f min = %.3f hrs\n",time_elapsed,sec_per_step,ETA,ETA_hrs);
			
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
            printf("RD avg =              %.5f +- %.5f K (LJ = %.4f, LRC = %.4f, LRC_self = %.4f)\n",
                system.stats.rd.average, system.stats.rd.sd, system.stats.lj.average, system.stats.lj_lrc.average, system.stats.lj_self_lrc.average);
			printf("ES avg =              %.5f +- %.5f K (real = %.4f, recip = %.4f, self = %.4f)\n",
                system.stats.es.average, system.stats.es.sd, system.stats.es_real.average, system.stats.es_recip.average, system.stats.es_self.average);
			printf("Polar avg =           %.5f +- %.5f K\n",system.stats.polar.average, system.stats.polar.sd);
			printf("Total potential avg = %.5f +- %.5f K\n",system.stats.potential.average, system.stats.potential.sd);
			printf("Volume avg  = %.2f +- %.2f A^3 = %.2f nm^3\n",system.stats.volume.average, system.stats.volume.sd, system.stats.volume.average/1000.0);
			printf("Density avg = %.6f +- %.3f g/mL = %6f g/L \n",system.stats.density.average, system.stats.density.sd, system.stats.density.average*1000.0); 
			if (system.constants.ensemble == "uvt") {
                printf("wt %% = %.4f +- %.4f %%; wt %% ME = %.4f +- %.4f %% = %.4f mmol/g\n",system.stats.wtp.average, system.stats.wtp.sd, system.stats.wtpME.average, system.stats.wtpME.sd, system.stats.wtpME.average * 10 / (system.proto.mass * 1000 * system.constants.NA));
                printf("Qst avg = %.5f +- %.5f kJ/mol\n", system.stats.qst.average, system.stats.qst.sd);
            }
            if (system.constants.ensemble != "npt") {
                printf("Chemical potential avg = %.4f +- %.4f K / sorbate molecule \n", system.stats.chempot.average, system.stats.chempot.sd); 
            }
            printf("Compressibility factor Z avg = %.6f +- %.6f (for homogeneous gas %s) \n",system.stats.z.average, system.stats.z.sd, system.proto.name.c_str());
            printf("N_movables avg = %.3f +- %.3f; N_molecules = %i; N_movables = %i; N_sites = %i\n",
                system.stats.Nmov.average, system.stats.Nmov.sd, (int)system.molecules.size(), system.stats.count_movables, system.constants.total_atoms);

            if (system.constants.dist_within_option == "on") {
                printf("N of %s within %.5f A of origin: %.5f +- %.3f (actual: %i)\n", system.constants.dist_within_target.c_str(), system.constants.dist_within_radius, system.stats.dist_within.average, system.stats.dist_within.sd, (int)system.stats.dist_within.value);
            }

            printf("--------------------\n\n");

            // CONSOLIDATE ATOM AND MOLECULE PDBID's
            // quick loop through all atoms to make PDBID's pretty (1->N)
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
            writeXYZ(system,system.constants.output_traj,frame,t,0);
            frame++;
            writePDB(system, system.constants.restart_pdb);
            if (system.constants.pdb_traj_option == "on")
                writePDBtraj(system, system.constants.restart_pdb, system.constants.output_traj_pdb, t);    
            writeThermo(system, system.stats.potential.average, 0.0, 0.0, system.stats.potential.average, system.stats.density.average*1000, system.constants.temp, system.constants.pres, t);
            if (system.stats.radial_dist == "on") {
                radialDist(system);
                writeRadialDist(system);        
            }

            // count the corrtime occurences.
            corrtime_iter++;

		} // END IF CORRTIME	
	} // END MC STEPS LOOP.

	// FINAL EXIT OUTPUT
    if (system.constants.ensemble == "npt") {
	    printf("Final basis parameters: \n");
        system.pbc.printBasis();
    }
	printf("Insert accepts:        %i\n", system.stats.insert_accepts);
	printf("Remove accepts:        %i\n", system.stats.remove_accepts);
	printf("Displace accepts:      %i\n", system.stats.displace_accepts);
	printf("Volume change accepts: %i\n", system.stats.volume_change_accepts);	

	std::chrono::steady_clock::time_point end= std::chrono::steady_clock::now();
        time_elapsed = (std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()) /1000000.0;
	printf("Total wall time = %f s\n",time_elapsed);

    if (system.constants.potential_form == "ljespolar") {
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
    
        // write initial XYZ
        writeXYZ(system,system.constants.output_traj, 1, 0, 0);	
        int frame = 2; // weird way to initialize but it works for the output file.
        // and initial PDB
        writePDB(system,system.constants.restart_pdb);
            if (system.constants.pdb_traj_option == "on")
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
    } else {
        // otherwise use temperature as default via v_RMS
        // default temp is zero so init. vel's will be 0 if no temp is given.
        double v_init = sqrt(8.0 * system.constants.R * system.constants.temp / (M_PI*system.proto.mass*system.constants.NA)) / 1e5; // A/fs
        double v_component = sqrt(v_init*v_init / 3.0);

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
    system.constants.md_vel_goal = sqrt(system.constants.md_init_vel*system.constants.md_init_vel/3.0);
     // end initial velocities


	double dt = system.constants.md_dt; // * 1e-15; //0.1e-15; // 1e-15 is one femptosecond.
	double tf = system.constants.md_ft; // * 1e-15; //100e-15; // 100,000e-15 would be 1e-9 seconds, or 1 nanosecond. 
	int total_steps = floor(tf/dt);
	int count_md_steps = 1;
	
        printf("\n| ========================================= |\n");
        printf("|  BEGINNING MOLECULAR DYNAMICS SIMULATION  |\n");
        printf("| ========================================= |\n\n");
	

	// begin timing for steps
	std::chrono::steady_clock::time_point begin_steps = std::chrono::steady_clock::now();
	for (double t=dt; t < tf; t=t+dt) {
		//printf("%f\n",t);
		integrate(system,dt);
		
		if (count_md_steps % system.constants.md_corrtime == 0) {  // print every x steps 
				
            // get KE and PE and T at this step.
            double* ETarray = calculateEnergyAndTemp(system, t);
            double KE = ETarray[0];
            double PE = ETarray[1];
            double TE = KE+PE;
            double Temp = ETarray[2];
            double v_avg = ETarray[3];
            double Ek = ETarray[4];
            double Klin = ETarray[5];
            double Krot = ETarray[6];
            double pressure = ETarray[7]; // only good for NVT. Frenkel p84

            // PRESSURE (my pathetic nRT/V method)
            //double pressure = TE/system.constants.volume * system.constants.kb * 1e30 * 9.86923e-6; // P/V to atm

			// PRINT OUTPUT
			std::chrono::steady_clock::time_point end= std::chrono::steady_clock::now();
			time_elapsed = (std::chrono::duration_cast<std::chrono::microseconds>(end - begin_steps).count()) /1000000.0;
			sec_per_step = time_elapsed/(double)count_md_steps;				
			double progress = (((float)count_md_steps)/(float)total_steps*100);
			double ETA = ((time_elapsed*(float)total_steps/(float)count_md_steps) - time_elapsed)/60.0;
			double ETA_hrs = ETA/60.0;
       
            printf("MOLECULAR DYNAMICS\n");
            printf("testing angular velocity\n");
            printf("%s %s\n",system.constants.jobname.c_str(),argv[1]);
            printf("Input atoms: %s\n",system.constants.atom_file.c_str());
            printf("ENSEMBLE: %s\n",system.constants.ensemble.c_str());
            printf("Input T: %.3f K; Input P: %.3f atm\n",system.constants.temp, system.constants.pres);
            printf("Step: %i / %i; Progress = %.3f%%; Realtime = %.2f fs\n",count_md_steps,total_steps,progress,t);
			printf("Time elapsed = %.2f s = %.3f sec/step; ETA = %.3f min = %.3f hrs\n",time_elapsed,sec_per_step,ETA,ETA_hrs);
			printf("KE: %.3f K (lin: %.3f , rot: %.3e ); PE: %.3f K; Total E: %.3f K; \nKE(equipartition) = %.3f K\nEmergent T: %.3f K; Average v = %.5f A/fs; v_init = %.5f A/fs\nEmergent Pressure: %.3f atm\n", 
            KE, Klin, Krot, PE, TE, Ek, 
            Temp, v_avg, system.constants.md_init_vel,
            pressure);			

			printf("--------------------\n\n");

            // WRITE OUTPUT FILES 
			writeXYZ(system,system.constants.output_traj,frame,count_md_steps,t);
            frame++;
            writeThermo(system, TE, Klin, Krot, PE, 0.0, Temp, pressure, count_md_steps); 
            writePDB(system,system.constants.restart_pdb);	
            if (system.constants.pdb_traj_option == "on") 
                writePDBtraj(system,system.constants.restart_pdb, system.constants.output_traj_pdb, count_md_steps);
            if (system.stats.radial_dist == "on") {
                radialDist(system);
                writeRadialDist(system);
            }
		}
		count_md_steps++;
	} // end MD timestep loop
	}
}        
