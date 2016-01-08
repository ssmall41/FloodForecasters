#include <stdio.h>
#include <time.h>
#include <libpq-fe.h>
#include <string.h>
#include <unistd.h>
#include "asynch_interface.h"
#include "forecaster_methods.h"

int my_rank;
int np;

typedef struct CustomParams
{
	unsigned int ID;
	int offset;
} CustomParams;

typedef struct CustomParamsMaps
{
	unsigned int forecast_time;
	unsigned int period;
} CustomParamsMaps;

void UploadPeakflows(asynchsolver* asynch,unsigned int wait_time);

int Output_Linkid(double t,VEC* y_i,VEC* global_params,VEC* params,int state,void* user);
int Output_Timestamp(double t,VEC* y_i,VEC* global_params,VEC* params,int state,void* user);
void OutputPeakflow_Forecast_Maps(unsigned int ID,double peak_time,VEC* peak_value,VEC* params,VEC* global_params,double conversion,unsigned int area_idx,void* user,char* buffer);
void Init_Output_User_forecastparams(asynchsolver* asynch);
void Free_Output_User_forecastparams(asynchsolver* asynch);
void Set_Output_User_forecastparams(asynchsolver* asynch,unsigned int offset);

void Init_Output_PeakflowUser_Offset(asynchsolver* asynch);
void Free_Output_PeakflowUser_Offset(asynchsolver* asynch);
void Set_Output_PeakflowUser_Offset(asynchsolver* asynch,unsigned int forecast_time,unsigned int period);


int main(int argc,char* argv[])
{
	//Initialize MPI stuff
	MPI_Init(&argc,&argv);
	MPI_Comm_rank(MPI_COMM_WORLD,&my_rank);
	MPI_Comm_size(MPI_COMM_WORLD,&np);

	//Parse input
	if(argc < 7)
	{
		if(my_rank == 0)
			printf("Command line parameter required:\nA universal variable file (.gbl),\nA forecast file (.fcst),\nStart timestamp,\nEnd timestamp,\nExit filename,\nInit timestamp\n");
		MPI_Finalize();
		return 1;
	}

	//Check if we should upload files instead of database uploads for the hydrographs. Same for snapshots.
	int hydro_files = 0,snapshot_files = 0;
	char *hydro_additional = NULL,*snapshot_additional = NULL,*snapshot_file_location = NULL;
	if(argc > 7)
	{
		hydro_files =  atoi(argv[7]);
		hydro_additional = (char*) malloc(1024*sizeof(char));

		if(hydro_files || argc > 8)	//!!!! This is really tacky. Need a better way to pass information in. !!!!
		{
			if(argc < 9)
			{
				if(my_rank == 0)
					printf("Command line parameter required:\nA universal variable file (.gbl),\nA forecast file (.fcst),\nStart timestamp,\nEnd timestamp,\nExit filename,\nInit timestamp,\nUpload hydrograph files flag,\nUpload snapshot files flag,\nSnapshot file location\n");
				MPI_Finalize();
				return 1;
			}
			snapshot_files = atoi(argv[8]);
			snapshot_additional = (char*) malloc(1024*sizeof(char));
			snapshot_file_location = argv[9];	//!!!! This also holds the server info for uploading hydro files !!!!

			if(snapshot_files == hydro_files && hydro_files)
				printf("Warning: uploading both snapshot files and hydrograph files to a database. This may not be supported yet...\n");
		}
	}

	//Declare variables
	unsigned int i,j,k,current_offset;
	int isnull;
	double total_time = 0.0;
	time_t start,start2,stop;
	asynchsolver* asynch;
	PGresult *res;
	MPI_Status status;
	char* query = (char*) malloc(1024*sizeof(char));
	Link* current;

	if(my_rank == 0)
		printf("\nBeginning initialization...\n*****************************\n");
	MPI_Barrier(MPI_COMM_WORLD);
	start = time(NULL);

	//Init asynch object and the river network
	asynch = Asynch_Init(MPI_COMM_WORLD,&argc,&argv);
	if(my_rank == 0)	printf("Reading global file...\n");
	Asynch_Parse_GBL(asynch,argv[1]);
	Asynch_Set_Total_Simulation_Time(asynch,(atoi(argv[4]) - atoi(argv[3]))/60.0);
	Asynch_Set_Init_Timestamp(asynch,atoi(argv[6]));

	//Load Forecast related data
	ForecastData* Forecaster = Init_ForecastData(argv[2],asynch->GlobalVars->string_size);
	if(!Forecaster)
		MPI_Abort(MPI_COMM_WORLD,1);

	//Check if there is work to do
	if(my_rank == 0)
	{
		isnull = 1;
		ConnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_FORCING_START]); //!!!! Assumes forecaster_idx is 0 !!!!
		sprintf(query,Forecaster->rainmaps_db->queries[0],atoi(argv[3])+ 60 * (unsigned int) rint(asynch->forcings[0]->file_time) * (Forecaster->num_rainsteps-1));
		res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_FORCING_START]->conn,query);
		CheckResError(res,"checking for new rainfall data");
		isnull = PQgetisnull(res,0,0);
		PQclear(res);
		DisconnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_FORCING_START]);
	}
	MPI_Bcast(&isnull,1,MPI_INT,0,MPI_COMM_WORLD);

	if(isnull)
	{
		if(my_rank == 0)	printf("No new forcing. Exiting...\n");
		MPI_Finalize();
		return 0;
	}

	//Load the system
	double forecast_time = Forecaster->forecast_window;
	double holder = Asynch_Get_Total_Simulation_Time(asynch);
	double longest = (holder < forecast_time) ? forecast_time : holder;
	Asynch_Set_Total_Simulation_Time(asynch,longest);
	if(my_rank == 0)	printf("Loading network...\n");
	Asynch_Load_Network(asynch);
	if(my_rank == 0)	printf("Partitioning network...\n");
	Asynch_Partition_Network(asynch);
	if(my_rank == 0)	printf("Loading parameters...\n");
	Asynch_Load_Network_Parameters(asynch,0);
	if(my_rank == 0)	printf("Reading dam and reservoir data...\n");
	Asynch_Load_Dams(asynch);
	if(my_rank == 0)	printf("Setting up numerical error data...\n");
	Asynch_Load_Numerical_Error_Data(asynch);
	if(my_rank == 0)	printf("Initializing model...\n");
	Asynch_Initialize_Model(asynch);
	if(my_rank == 0)	printf("Loading initial conditions...\n");
	Asynch_Load_Initial_Conditions(asynch);
	if(my_rank == 0)	printf("Loading forcings...\n");
	Asynch_Load_Forcings(asynch);
	if(my_rank == 0)	printf("Loading output data information...\n");
	Asynch_Load_Save_Lists(asynch);
	if(my_rank == 0)	printf("Finalizing network...\n");
	Asynch_Finalize_Network(asynch);
	if(my_rank == 0)	printf("Calculating initial step sizes...\n");
	Asynch_Calculate_Step_Sizes(asynch);
	Asynch_Set_Total_Simulation_Time(asynch,holder);

	//Setup output for link id, if needed
	if(!hydro_files)
	{
		int setup_id = Asynch_Check_Output(asynch,"LinkID");
		int setup_timestamp = Asynch_Check_Output(asynch,"Timestamp");
		if( (setup_id || setup_timestamp) != 0)
		{
			if(my_rank == 0)	printf("[%i]: Forecaster needs LinkID (%i), and Timestamp (%i).\n",my_rank,setup_id,setup_timestamp);
			MPI_Abort(MPI_COMM_WORLD,1);
		}
	}
	Init_Output_User_forecastparams(asynch);
	if(!hydro_files)
	{
		Asynch_Set_Output(asynch,"LinkID",ASYNCH_INT,(void (*)(double,VEC*,VEC*,VEC*,int,void*)) &Output_Linkid,NULL,0);
		Asynch_Set_Output(asynch,"Timestamp",ASYNCH_INT,(void (*)(double,VEC*,VEC*,VEC*,int,void*)) &Output_Timestamp,NULL,0);
	}

	//Setup the peakflow information for maps
	int setup_peakflow_maps = Asynch_Check_Peakflow_Output(asynch,"Forecast_Maps");
	if(setup_peakflow_maps != 0)
	{
		if(my_rank == 0)	printf("[%i]: Forecaster with maps needs Forecast_Maps (%i) for peakflows.\n",my_rank,setup_peakflow_maps);
		MPI_Abort(MPI_COMM_WORLD,1);
	}
	Init_Output_PeakflowUser_Offset(asynch);
	Asynch_Set_Peakflow_Output(asynch,"Forecast_Maps",(void (*)(unsigned int,double,VEC*,VEC*,VEC*,double,unsigned int,void*,char*)) &OutputPeakflow_Forecast_Maps);

	//Get some values about the river system
	unsigned int N = Asynch_Get_Number_Links(asynch);
	unsigned int my_N = Asynch_Get_Local_Number_Links(asynch);
	char dump_filename[asynch->GlobalVars->string_size];

	//Create halt file
	CreateHaltFile(Forecaster->halt_filename);

	//Find the index of the forcing to use for forecasting
	unsigned int forecast_idx = Forecaster->forecasting_forcing;
	if(forecast_idx >= asynch->GlobalVars->num_forcings)
	{
		if(my_rank == 0)	printf("[%i]: Error: No forecasting forcing set.\n",my_rank);
		MPI_Abort(MPI_COMM_WORLD,1);
	}

	//Set new start and end times
	unsigned int override_starttime = atoi(argv[3]);
	unsigned int override_endtime = atoi(argv[4]);
	for(i=0;i<asynch->GlobalVars->num_forcings;i++)
		Asynch_Set_First_Rainfall_Timestamp(asynch,override_starttime,i);
	Asynch_Set_Last_Rainfall_Timestamp(asynch,override_endtime,forecast_idx);

	//Reserve space for backups
	VEC** backup = (VEC**) malloc(N*sizeof(VEC*));
	for(i=0;i<N;i++)
	{
		if(asynch->assignments[i] == my_rank || asynch->getting[i] == 1)
			backup[i] = v_get(asynch->sys[i]->dim);
		else
			backup[i] = NULL;
	}

	if(my_rank == 0)
	{
		printf("\nModel type is %u.\nGlobal parameters are:\n",asynch->GlobalVars->type);
		Print_Vector(asynch->GlobalVars->global_params);
		printf("\n");
	}

	MPI_Barrier(MPI_COMM_WORLD);
	stop = time(NULL);
	total_time = difftime(stop,start);
	if(my_rank == 0)	printf("Finished initializations. Total time: %f\n",total_time);
	MPI_Barrier(MPI_COMM_WORLD);
	sleep(1);

	//Make sure everyone is good before getting down to it...
	printf("Process %i (%i total) is good to go with %i links.\n",my_rank,np,my_N);
	MPI_Barrier(MPI_COMM_WORLD);
	start = time(NULL);

	//Make the initial solve
	Asynch_Advance(asynch,0);

	//Stop the clock
	MPI_Barrier(MPI_COMM_WORLD);
	stop = time(NULL);
	total_time += difftime(stop,start);

	//Output some data
	if(my_rank == 0)
	{
		printf("%i: The answer at ID %i at time %.12f is\n",my_rank,asynch->sys[asynch->my_sys[0]]->ID,asynch->sys[asynch->my_sys[0]]->last_t);
		Print_Vector(asynch->sys[asynch->my_sys[0]]->list->tail->y_approx);
		printf("Total time for calculations: %f\n",difftime(stop,start));
	}

	//Check if there is a schema used for the hydrograph archive
	int place,tablename_len = strlen(asynch->GlobalVars->hydro_table);
	char schema[128]; schema[0] = '\0';
	for(place=tablename_len-1;place>-1;place--)
	{
		if(asynch->GlobalVars->hydro_table[place] == '.')
		{
			asynch->GlobalVars->hydro_table[place] = '\0';
			strcpy(schema,asynch->GlobalVars->hydro_table);
			asynch->GlobalVars->hydro_table[place] = '.';
			schema[place] = '.';
			schema[place+1] = '\0';
			break;
		}
	}

	//Begin persistent calculations
	if(my_rank == 0)
		printf("\n\n===================================\nBeginning persistent calculations\n===================================\n");
	fflush(stdout);
	MPI_Barrier(MPI_COMM_WORLD);

	//Make some initializations and checks
	//unsigned int history_time = 5*24*60*60;	//Amount of history to store for hydrographs and peakflows
	//short unsigned int hr1 = 0;	//Hour of the day to perform maintainance on database
	//short unsigned int hr2 = 12;	//Hour of the day to perform maintainance on database
	unsigned int wait_time = 120;	//Time to sleep if no rainfall data is available
	//double forecast_time = 10.0*24*60;	//Time (mins) in future to make forecasts
	unsigned int num_tables = 10;
	unsigned int db_retry_time = 5;	//Time (secs) to wait if a database error occurs
	unsigned int num_future_peakflow_times = 9;
	double future_peakflow_times[] = {60.0, 180.0, 360.0, 720.0, 1440.0, 2880.0, 4320.0, 5760.0, 7200.0};
	//unsigned int num_rainsteps = 3;	//Number of rainfall intensities to use for the next forecast
	unsigned int num_rainsteps = Forecaster->num_rainsteps;	//Number of rainfall intensities to use for the next forecast
	//if(my_rank == 0 && asynch->GlobalVars->increment < num_rainsteps + 3)
	if(my_rank == 0 && asynch->forcings[forecast_idx]->increment < num_rainsteps + 3)
		printf("Warning: Increment for rain should probably be %u.\n",num_rainsteps + 3);
	asynch->forcings[forecast_idx]->increment = num_rainsteps;	//!!!! Not necessary, but makes me feel better. The solvers should really not do the last step where they download nothing. !!!!
	double db_stepsize = asynch->forcings[forecast_idx]->file_time,t;

	unsigned int nextraintime,repeat_for_errors,nextforcingtime;
	short int halt = 0;
	unsigned int last_file = asynch->forcings[forecast_idx]->last_file;
	unsigned int first_file = asynch->forcings[forecast_idx]->first_file;
	k = 0;
	for(i=0;i<N;i++)
		if(backup[i] != NULL)	v_copy(asynch->sys[i]->list->tail->y_approx,backup[i]);

	double simulation_time_with_data = 0.0;
	simulation_time_with_data = max(simulation_time_with_data,asynch->forcings[forecast_idx]->file_time * Forecaster->num_rainsteps);

	//Set peakflow output
	Asynch_Prepare_Peakflow_Output(asynch);

	//Setup temp files
	Set_Output_User_forecastparams(asynch,first_file);
	Asynch_Set_Total_Simulation_Time(asynch,forecast_time);
	Asynch_Prepare_Temp_Files(asynch);

	//Make some initializations to the database
	if(my_rank == 0)
	{
		printf("Making initializations to tables.\n");
		start = time(NULL);

		if(!hydro_files)
		{
			//Connect to hydrograph database
			ConnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);

			//Make sure the hydrographs table exists
			sprintf(query,"SELECT 1 FROM pg_class WHERE relname='%s';",asynch->GlobalVars->hydro_table);
			res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
			if(!PQntuples(res))
			{
				PQclear(res);
				sprintf(query,"CREATE TABLE %s(link_id int,time int,ratio real,discharge real); ALTER TABLE %s SET (autovacuum_enabled = false, toast.autovacuum_enabled = false);",asynch->GlobalVars->hydro_table,asynch->GlobalVars->hydro_table);
				res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
				CheckResError(res,"creating hydrographs table");
			}
			else
			{
				PQclear(res);
				sprintf(query,"TRUNCATE %s;",asynch->GlobalVars->hydro_table);
				res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
				CheckResError(res,"truncating hydrographs table");
			}
			PQclear(res);
			DisconnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);

			//Make sure the hydroforecast tables are set correctly
			CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_hydroforecast","forecast_time",schema);

			//Clear the future hydrographs in archive
			DeleteFutureValues(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT],num_tables,asynch->GlobalVars,"archive_hydroforecast",Forecaster->model_name,first_file,1,schema);
		}

		//Make sure the peakflow tables are set correctly
		CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_PEAK_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_peakflows","forecast_time",schema);

		//Clear all future peakflows
		DeleteFutureValues(asynch->db_connections[ASYNCH_DB_LOC_PEAK_OUTPUT],num_tables,asynch->GlobalVars,"archive_peakflows",Forecaster->model_name,first_file,1,schema);

		//Make sure the map tables are set correctly
		CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_SNAPSHOT_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_maps","forecast_time",schema);

		//Clear all future maps
		DeleteFutureValues(asynch->db_connections[ASYNCH_DB_LOC_SNAPSHOT_OUTPUT],num_tables,asynch->GlobalVars,"archive_maps",Forecaster->model_name,first_file,0,schema);

		stop = time(NULL);
		printf("Total time to initialize tables: %.2f.\n",difftime(stop,start));
	}

	MPI_Barrier(MPI_COMM_WORLD);

	//Start the main loop
	while(!halt)
	{
		if(my_rank == 0)
		{
			time_t now = time(NULL);
			struct tm* now_info = localtime(&now);
			printf("\n\nPass %u\n",k);
			printf("Current time is %s",asctime(now_info));
		}

		//Clear buffers
		Flush_TransData(asynch->my_data);

		//Make some initializations
		//asynch->forcings[forecast_idx]->raindb_start_time = last_file;								//!!!! This all assumes one forcing from db !!!!
		first_file = last_file;
		last_file = last_file + (unsigned int) asynch->forcings[forecast_idx]->file_time * 60 * num_rainsteps;
		nextforcingtime = first_file + 60 * (unsigned int) rint(asynch->forcings[forecast_idx]->file_time) * (num_rainsteps-1);	//This is the actual timestamp of the last needed forcing data. This will be downloaded (unlike last_file)
		//nextforcingtime = first_file + 60 * (unsigned int) rint(asynch->forcings[forecast_idx]->file_time) * num_rainsteps;

		//Reset each link
		Asynch_Set_System_State(asynch,0.0,backup);
		Set_Output_User_forecastparams(asynch,first_file);
		Set_Output_PeakflowUser_Offset(asynch,first_file,first_file);
		Asynch_Write_Current_Step(asynch);
		Asynch_Set_Forcing_State(asynch,forecast_idx,0.0,first_file,last_file);	//!!!! Seems redundant with next loop !!!!

		for(i=0;i<asynch->GlobalVars->num_forcings;i++)	//Set any other database forcings to begin at first_file
		{
			if(asynch->forcings[i]->flag == 3)
				Asynch_Set_Forcing_State(asynch,i,0.0,first_file,asynch->forcings[i]->last_file);
		}

		//Check if a vacuum should be done
		//This will happen at hr1
		if(my_rank == 0)
		{
			if(!hydro_files)	CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_hydroforecast","forecast_time",schema);
			CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_PEAK_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_peakflows","forecast_time",schema);
			CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_SNAPSHOT_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_maps","forecast_time",schema);
		}

		//Make sure all buffer flushing is done
		MPI_Barrier(MPI_COMM_WORLD);

		//Find the next time where rainfall occurs
		if(my_rank == 0)
		{
			ConnectPGDB(Forecaster->rainmaps_db);

			//Find the next rainfall time
			time(&start);
			sprintf(query,Forecaster->rainmaps_db->queries[0],nextforcingtime);
			res = PQexec(Forecaster->rainmaps_db->conn,query);
			CheckResError(res,"checking for new rainfall data");
			time(&stop);
			printf("Total time to check for new rainfall data: %f.\n",difftime(stop,start));
			isnull = PQgetisnull(res,0,0);

			PQclear(res);
			DisconnectPGDB(Forecaster->rainmaps_db);
		}
		MPI_Bcast(&isnull,1,MPI_INT,0,MPI_COMM_WORLD);

		if(isnull)
		{
			if(my_rank == 0)
			{
				printf("No rainfall values returned from SQL database for forcing %u. %u %u\n",forecast_idx,last_file,isnull);
				if(!hydro_files)	CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_hydroforecast","forecast_time",schema);
				CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_PEAK_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_peakflows","forecast_time",schema);
				CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_SNAPSHOT_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_maps","forecast_time",schema);
			}

			halt = CheckFinished(Forecaster->halt_filename);
			if(!halt)	fflush(stdout);
		}

		if(halt || isnull)	break;

		//Read in next set of rainfall data

		//Initialize some data for the first phase of calculations
		Asynch_Set_Total_Simulation_Time(asynch,simulation_time_with_data);		// !!!! This may not work for multiple forcings for forecasting. How do you handle different time resolutions? !!!!
		current_offset = first_file;
		Set_Output_User_forecastparams(asynch,current_offset);
		Set_Output_PeakflowUser_Offset(asynch,current_offset,current_offset);

		MPI_Barrier(MPI_COMM_WORLD);
		time(&start);
if(my_rank == 0)
printf("first: %u last: %u\n",first_file,last_file);

		Asynch_Advance(asynch,1);

		MPI_Barrier(MPI_COMM_WORLD);
		if(my_rank == 0)
		{
			time(&stop);
			printf("Time for first phase calculations: %.2f\n",difftime(stop,start));
		}

		//Flush communication buffers
		Flush_TransData(asynch->my_data);

		//Reset the links (mostly) and make a backup for the second phase
		//!!!! Need routine for this !!!!
		for(i=0;i<N;i++)	//Set time to 0.0
		{
			current = asynch->sys[i];
			if(current->list != NULL)
			{
				while(current->current_iterations > 1)
				{
					Remove_Head_Node(current->list);
					(current->current_iterations)--;
				}

				current->steps_on_diff_proc = 1;
				current->iters_removed = 0;
				current->rejected = 0;
				if(current->numparents == 0)	current->ready = 1;
				else				current->ready = 0;
				v_copy(current->list->head->y_approx,backup[i]);
			}
		}

		//Upload a snapshot to the database
		if(my_rank == 0)
			CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_SNAPSHOT_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_maps","forecast_time",schema);
		MPI_Barrier(MPI_COMM_WORLD);
		sprintf(dump_filename,"%u",first_file);
		Asynch_Take_System_Snapshot(asynch,dump_filename);	//Send snapshot to database

		if(snapshot_files)	//See if a .rec file should be uploaded created and uploaded somewhere
		{
			sprintf(snapshot_additional,"%s_%u.rec",snapshot_file_location,first_file);
			Asynch_Set_Snapshot_Output_Name(asynch,snapshot_additional);
			DataDump2(asynch->sys,asynch->N,asynch->assignments,asynch->GlobalVars,NULL,NULL);	//!!!! Dirty... !!!!

			if(my_rank == 0)
			{
				while(SendFilesTo51(asynch->GlobalVars->dump_loc_filename,"/data/ifc_01_maps/"))
				{
					printf("[%i]: Error scp'ing snapshot file. Retrying...\n",my_rank);
					sleep(5);
				}
			}
		}


		//Make second phase calculations. Peakflow data will be uploaded several times.
		MPI_Barrier(MPI_COMM_WORLD);
		time(&start);

		Asynch_Deactivate_Forcing(asynch,forecast_idx);

		for(i=0;i<num_future_peakflow_times;i++)
		{
			//t = future_peakflow_times[i] + db_stepsize*num_rainsteps;
			t = asynch->sys[asynch->my_sys[0]]->last_t;
			//Asynch_Set_Total_Simulation_Time(asynch,t);
			Asynch_Set_Total_Simulation_Time(asynch,future_peakflow_times[i] + db_stepsize*num_rainsteps);
			Asynch_Reset_Peakflow_Data(asynch);
			Set_Output_PeakflowUser_Offset(asynch,current_offset,current_offset + (unsigned int) (60.0*t+0.1));
			Asynch_Advance(asynch,1);
			if(my_rank == 0)
				CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_PEAK_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_peakflows","forecast_time",schema);
			MPI_Barrier(MPI_COMM_WORLD);
			UploadPeakflows(asynch,db_retry_time);
		}

		Asynch_Reset_Peakflow_Data(asynch);
		Asynch_Set_Total_Simulation_Time(asynch,forecast_time);
		Asynch_Advance(asynch,1);
		Asynch_Activate_Forcing(asynch,forecast_idx);

		//Flush communication buffers	!!!! This keeps biting me in the ass. Put in Asynch_Advance. !!!!
		Flush_TransData(asynch->my_data);

		MPI_Barrier(MPI_COMM_WORLD);
		if(my_rank == 0)
		{
			time(&stop);
			printf("Time for second phase calculations: %.2f\n",difftime(stop,start));
		}

		//Output some data
		if(my_rank == 0)
		{
			printf("[%i]: The answer at ID %i at time %.12f is\n",my_rank,asynch->sys[asynch->my_sys[0]]->ID,asynch->sys[asynch->my_sys[0]]->last_t);
			Print_Vector(asynch->sys[asynch->my_sys[0]]->list->tail->y_approx);
		}

		//Upload the hydrographs to the database ********************************************************************************************
		MPI_Barrier(MPI_COMM_WORLD);
		start = time(NULL);

		//Adjust the table hydrographs
		if(my_rank == 0 && !hydro_files)
		{
			CheckPartitionedTable(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT],asynch->GlobalVars,Forecaster,num_tables,"archive_hydroforecast","forecast_time",schema);
			ConnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);
			sprintf(query,"TRUNCATE %s;",asynch->GlobalVars->hydro_table);
			res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
			CheckResError(res,"deleting hydrographs");
			PQclear(res);
			DisconnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);
		}
		MPI_Barrier(MPI_COMM_WORLD);

		if(hydro_files)
			sprintf(hydro_additional,"%u",first_file);

		repeat_for_errors = Asynch_Create_Output(asynch,hydro_additional);
		while(repeat_for_errors > 0)
		{
			if(my_rank == 0)	printf("[%i]: Attempting resend of hydrographs data (%i).\n",my_rank,repeat_for_errors);
			sleep(5);
			repeat_for_errors = Asynch_Create_Output(asynch,hydro_additional);
		}

		//Call functions *********************************************************************************************************************

		if(!hydro_files)
		{
			if(my_rank == 0)
			{
				//Connect to database
				ConnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);

				//Functions for displaying data on IFIS
				if(Forecaster->ifis_display)
				{
					//Stage
					repeat_for_errors = 1;
					while(repeat_for_errors)
					{
						repeat_for_errors = 0;
						sprintf(query,"SELECT get_stages_%s();",Forecaster->model_name);
						res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
						repeat_for_errors = repeat_for_errors || CheckResError(res,"calling stage function");
						PQclear(res);
						if(repeat_for_errors)
						{
							printf("[%i]: Attempting to call stage function again...\n",my_rank);
							sleep(5);
							CheckConnConnection(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);
						}
					}
/*
					//Warnings
					repeat_for_errors = 1;
					while(repeat_for_errors)
					{
						repeat_for_errors = 0;
						sprintf(query,"SELECT update_warnings_%s();",Forecaster->model_name);
						res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
						repeat_for_errors = repeat_for_errors || CheckResError(res,"calling warnings function");
						PQclear(res);
						if(repeat_for_errors)
						{
							printf("[%i]: Attempting to call warning function again...\n",my_rank);
							sleep(5);
							CheckConnConnection(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);
						}
					}
*/
				}

				//Stage archive
				repeat_for_errors = 1;
				while(repeat_for_errors)
				{
					repeat_for_errors = 0;
					sprintf(query,"ALTER TABLE master_archive_hydroforecast_%s ALTER COLUMN forecast_time SET DEFAULT %u;",Forecaster->model_name,current_offset);
					res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
					repeat_for_errors = repeat_for_errors || CheckResError(res,"setting default value");
					PQclear(res);

					sprintf(query,"SELECT copy_to_archive_hydroforecast_%s();",Forecaster->model_name);
					res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
					repeat_for_errors = repeat_for_errors || CheckResError(res,"calling stage archive function");
					PQclear(res);

					sprintf(query,"ALTER TABLE master_archive_hydroforecast_%s ALTER COLUMN forecast_time DROP DEFAULT;",Forecaster->model_name);
					res = PQexec(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]->conn,query);
					repeat_for_errors = repeat_for_errors || CheckResError(res,"dropping default value");
					PQclear(res);

					if(repeat_for_errors)
					{
						printf("[%i]: Attempting to call stage archive function again...\n",my_rank);
						sleep(5);
						CheckConnConnection(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);
					}
				}

				//Disconnect
				DisconnectPGDB(asynch->db_connections[ASYNCH_DB_LOC_HYDRO_OUTPUT]);
			}
		}
		else
		{
			sprintf(query,"%s_%s_%i.irad",asynch->GlobalVars->hydros_loc_filename,hydro_additional,my_rank);
			while(SendFilesTo51(query,snapshot_file_location))
			{
				printf("[%i]: Error scp'ing index file. Retrying...\n",my_rank);
				sleep(5);
			}

			sprintf(query,"%s_%s_%i.rad",asynch->GlobalVars->hydros_loc_filename,hydro_additional,my_rank);
			while(SendFilesTo51(query,snapshot_file_location))
			{
				printf("[%i]: Error scp'ing hydrograph file. Retrying...\n",my_rank);
				sleep(5);
			}
		}

		MPI_Barrier(MPI_COMM_WORLD);
		if(my_rank == 0)
		{
			time(&stop);
			printf("[%i]: Total time to transfer hydrograph data: %.2f\n",my_rank,difftime(stop,start));
		}

		fflush(stdout);
		MPI_Barrier(MPI_COMM_WORLD);

		//Check if program has received a terminate signal **********************************************************************************
		k++;
		halt = CheckFinished(Forecaster->halt_filename);
		if(halt)	first_file = last_file;	//This is to put the correct time in the exit file
	}

	//Save the last timestep
	if(my_rank == 0)
	{
		FILE* exitfile = fopen(argv[5],"w");
		if(!exitfile)	printf("Error opening exit file %s\n",argv[5]);
		else
		{
			fprintf(exitfile,"%i",first_file);
			fclose(exitfile);
		}
	}
	MPI_Barrier(MPI_COMM_WORLD);

	//Clean up **********************************************************************************************************************************
	if(hydro_additional)	free(hydro_additional);
	if(snapshot_additional)	free(snapshot_additional);
	free(query);
	for(i=0;i<N;i++)	v_free(backup[i]);
	free(backup);
	Free_ForecastData(&Forecaster);
	Asynch_Delete_Temporary_Files(asynch);
	Free_Output_PeakflowUser_Offset(asynch);
	Free_Output_User_forecastparams(asynch);
	Asynch_Free(asynch);
	MPI_Finalize();
	return 0;
}


//Calls the function to create peakflows. The function is called repeatedly until the data is sent.
void UploadPeakflows(asynchsolver* asynch,unsigned int wait_time)
{
	int repeat_for_errors;
	MPI_Barrier(MPI_COMM_WORLD);

	repeat_for_errors = Asynch_Create_Peakflows_Output(asynch);
	while(repeat_for_errors > 0)
	{
		if(my_rank == 0)	printf("[%i]: Attempting resend of peakflow data.\n",my_rank);
		sleep(wait_time);
		repeat_for_errors = Asynch_Create_Peakflows_Output(asynch);
	}
}

//Output functions ****************************************************************************
int Output_Linkid(double t,VEC* y_i,VEC* global_params,VEC* params,int state,void* user)
{
	CustomParams* forecastparams = (CustomParams*) user;
	return forecastparams->ID;
}

int Output_Timestamp(double t,VEC* y_i,VEC* global_params,VEC* params,int state,void* user)
{
	CustomParams* forecastparams = (CustomParams*) user;
	return (int)(round(t * 60.0 + forecastparams->offset) + 0.1);
}

void OutputPeakflow_Forecast_Maps(unsigned int ID,double peak_time,VEC* peak_value,VEC* params,VEC* global_params,double conversion,unsigned int area_idx,void* user,char* buffer)
{
	CustomParamsMaps* forecastparams = (CustomParamsMaps*) user;
	sprintf(buffer,"%u %u %.6e %u %u\n",ID,forecastparams->forecast_time + (unsigned int)(peak_time*60 + .1),peak_value->ve[0],forecastparams->forecast_time,forecastparams->period);
}


//Custom parameters for forecasting ***********************************************************
void Init_Output_User_forecastparams(asynchsolver* asynch)
{
	unsigned int i,my_N = asynch->my_N,*my_sys = asynch->my_sys;
	Link** sys = asynch->sys;

	for(i=0;i<my_N;i++)
		sys[my_sys[i]]->output_user = malloc(sizeof(CustomParams));
}

void Free_Output_User_forecastparams(asynchsolver* asynch)
{
	unsigned int i,my_N = asynch->my_N,*my_sys = asynch->my_sys;
	Link** sys = asynch->sys;

	for(i=0;i<my_N;i++)
	{
		free(sys[my_sys[i]]->output_user);
		sys[my_sys[i]]->output_user = NULL;
	}
}

void Set_Output_User_forecastparams(asynchsolver* asynch,unsigned int offset)
{
	unsigned int i,my_N = asynch->my_N,*my_sys = asynch->my_sys;
	Link** sys = asynch->sys;
	CustomParams* forecastparams;

	for(i=0;i<my_N;i++)
	{
		forecastparams = (CustomParams*) sys[my_sys[i]]->output_user;
		forecastparams->ID = sys[my_sys[i]]->ID;
		forecastparams->offset = offset;
	}
}

void Init_Output_PeakflowUser_Offset(asynchsolver* asynch)
{
	unsigned int i,my_N = asynch->my_N,*my_sys = asynch->my_sys;
	Link** sys = asynch->sys;

	for(i=0;i<my_N;i++)
		sys[my_sys[i]]->peakoutput_user = malloc(sizeof(CustomParamsMaps));
}

void Free_Output_PeakflowUser_Offset(asynchsolver* asynch)
{
	unsigned int i,my_N = asynch->my_N,*my_sys = asynch->my_sys;
	Link** sys = asynch->sys;

	for(i=0;i<my_N;i++)
	{
		free(sys[my_sys[i]]->peakoutput_user);
		sys[my_sys[i]]->peakoutput_user = NULL;
	}
}

void Set_Output_PeakflowUser_Offset(asynchsolver* asynch,unsigned int forecast_time,unsigned int period)
{
	unsigned int i,my_N = asynch->my_N,*my_sys = asynch->my_sys;
	Link** sys = asynch->sys;
	CustomParamsMaps* peak_params;

	for(i=0;i<my_N;i++)
	{
		peak_params = (CustomParamsMaps*) (sys[my_sys[i]]->peakoutput_user);
		peak_params->forecast_time = forecast_time;
		peak_params->period = period;
	}
}


