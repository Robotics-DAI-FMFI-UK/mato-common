#include <time.h>
#include <sys/time.h>

// ZED includes
#include <sl/Camera.hpp>

// Using std namespace
using namespace std;
using namespace sl;

const int MAX_CHAR = 128;

inline void setTxt(sl::float3 value, char* ptr_txt) {
	snprintf(ptr_txt, MAX_CHAR, "%3.4f; %3.4f; %3.4f", value.x, value.y, value.z);
}


long long msec()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return 1000L * tv.tv_sec + tv.tv_usec / 1000L;
}

int main(int argc, char **argv) {

    time_t tm;
    time(&tm);
    char logfilename[50];
    sprintf(logfilename, "/home/martin/position_log/log_%ld", (long)tm);
    FILE *f = fopen(logfilename, "w+");
    
    long long start_msec = msec();
    
    Camera zed;
    // Set configuration parameters for the ZED
    InitParameters init_parameters;
    init_parameters.coordinate_units = UNIT::METER;
    init_parameters.coordinate_system = COORDINATE_SYSTEM::RIGHT_HANDED_Y_UP;
    init_parameters.sdk_verbose = true;

    // Open the camera
    ERROR_CODE zed_open_state = zed.open(init_parameters);
    if (zed_open_state != ERROR_CODE::SUCCESS) {
        printf("Camera Open %d Exit Program.", zed_open_state);
        return EXIT_FAILURE;
    }

    auto camera_model = zed.getCameraInformation().camera_model;

    // Create text for GUI
    char text_rotation[MAX_CHAR];
    char text_translation[MAX_CHAR];

    // Set parameters for Positional Tracking
    PositionalTrackingParameters positional_tracking_param;
    positional_tracking_param.enable_area_memory = true;
    // enable Positional Tracking
    auto returned_state = zed.enablePositionalTracking(positional_tracking_param);
    if (returned_state != ERROR_CODE::SUCCESS) {
        printf("Enabling positionnal tracking failed: %d", returned_state);
        zed.close();
        return EXIT_FAILURE;
    }

    Pose camera_path;
    POSITIONAL_TRACKING_STATE tracking_state;
    
    while (1) {
        if (zed.grab() == ERROR_CODE::SUCCESS) {
            // Get the position of the camera in a fixed reference frame (the World Frame)
            tracking_state = zed.getPosition(camera_path, REFERENCE_FRAME::WORLD);

            if (tracking_state == POSITIONAL_TRACKING_STATE::OK) {
                // Get rotation and translation and displays it
                setTxt(camera_path.getEulerAngles(), text_rotation);
                setTxt(camera_path.getTranslation(), text_translation);
 	            // printf("angles: %s position: %s\n", text_rotation, text_translation);
 	            long tm = msec() - start_msec;	            
                fprintf(f, "%ld %s %s\n", tm, text_rotation, text_translation);
            }
            else
            {
				fprintf(f, "NOTOK\n");
				printf("NOTOK\n");
			}


        } else
            sleep_ms(1);
    }

    fclose(f);
    zed.disablePositionalTracking();
    zed.close();
    return EXIT_SUCCESS;
}


