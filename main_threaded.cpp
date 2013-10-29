
#include "Global.hpp"
#include "Config.hpp"
#include "v4l2_c.h"  // C-friendly version of CamObj.hpp so we can do multithreading
#include "FPSCounter.hpp"
#include "Threshold.hpp"
#include "PnPObj.hpp"
#include "BBBSerial.h"
#include "FlightDataRecording.hpp"
#include "ProfilerTool.h"  // ProfilerTool class to make profiling code easier


#include <opencv2/opencv.hpp>
#include <vector>
#include <iostream>
#include <iterator>
#include <pthread.h>    // multithreading



using namespace std;
using namespace cv;


/// ////////// Global Variables ////////// ///

// Camera parameters and buffers
struct v4l2Parms parms;
struct v4l2_buffer buf;
void* buff_ptr;
struct user_buffer_t{
    void* ptr[BUF_SZ];
    int buf_idx;
    int buf_last;
} user_buffer;

// OpenCV image objects
Mat frame(Size(IM_WIDTH,IM_HEIGHT),CV_8UC3);
Mat gray(Size(IM_WIDTH,IM_HEIGHT),CV_8UC1);
Mat binary(Size(IM_WIDTH,IM_HEIGHT),CV_8UC1);

// User-defined class objects
Threshold thresh;   // Does feature detection (thresholding wrapper class for customBlobDetector)
PnPObj PnP;         // Correlates LEDs w/ model points and computes UAV localization estimate
FPSCounter fps(15); // Computes real-time frame rate
/// ////////////////////////////////////// ///



/// Multithreading variable declarations
void* capture(void*);
void* processing(void*);
pthread_mutex_t framelock_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t done_saving_frame = PTHREAD_COND_INITIALIZER;
pthread_cond_t done_using_frame = PTHREAD_COND_INITIALIZER;


int main()
{
    // Print the system to stdout
    selfIdentifySystem();

    // Initialize threshold object
    initializeThresholdObj(thresh);
    cout << "Feature detector initialized." << endl;

    // Read camera intrinsic properties
    PnP.setCamProps(camDataFilename);
    cout << "Camera properties read-in." << endl;

    // Read 3-D model geometry
    PnP.setModelPoints(modelPointsFilename);
    cout << "3-D model geometry read-in." << endl;

    if(ARM)
    {
        BBBSerial Serial;
        cout << "Serial ports for BeagleBone Black initialized." << endl;
    }

    // Initialize any data recording specified by macros in Global.hpp
    initializeFlightDataRecorder();
    cout << "Flight data recorder initialized" << endl;

    // (Camera initialization will occur in the "capture" thread)


    // Initialize pthreads
    pthread_t capture_thread;
    pthread_t processing_thread;

    pthread_create(&capture_thread,NULL,capture,NULL);
    pthread_create(&processing_thread,NULL,processing,NULL);

    pthread_join(capture_thread,NULL); // Let the capture thread end the program

    cout << "Program ended" << endl;
    return 0;
}



void *capture(void*)
{

    // Configure camera properties
    v4l2_set_defalut_parms(&parms);
    char devName[15];
    sprintf(devName,"/dev/video%d",DEVICE);
    strcpy(parms.dev_name,devName);
    parms.width = IM_WIDTH;
    parms.height = IM_HEIGHT;
    parms.fps = IM_FPS;
    parms.timeout = 1;
    parms.customInitFcn = &custom_v4l2_init;

    // Open + initialize the camera for capturing
    v4l2_open_device(&parms);
    v4l2_init_device(&parms);
    v4l2_start_capturing(&parms);

    // Populate the circular buffer of pointers to image data
    for (int i=0; i<2*BUF_SZ; i++)
    {
        while(0 != v4l2_wait_for_data(&parms)){}
        v4l2_fill_buffer(&parms, &buf, &user_buffer.ptr[buf.index]);
        v4l2_queue_buffer(&parms, &buf);
    }
    cout << "Camera initialized and capturing.\n" << endl;



    //  Capture frames indefinitely
    while(1)
    {
        if ( 0 != v4l2_wait_for_data(&parms))   // calls select() -- waits for data to be ready
            continue;                           // retry on EAGAIN

        pthread_mutex_lock(&framelock_mutex);  // protect from other threads modifying image buffers
        user_buffer.buf_idx = buf.index;
        user_buffer.buf_last = (buf.index - 1) % BUF_SZ;
        v4l2_fill_buffer(&parms, &buf, &user_buffer.ptr[user_buffer.buf_idx]); // dequeue buffer
        v4l2_queue_buffer(&parms, &buf);
        pthread_cond_broadcast(&done_saving_frame);
        pthread_mutex_unlock(&framelock_mutex);

        // (Uncomment to display the rate at which images are *captured*)
//        double fps_cnt=fps.fps();
//        cout << "\r" "FPS: " << fps_cnt << flush;

    }

    v4l2_stop_capturing(&parms);
    v4l2_uninit_device(&parms);
    v4l2_close_device(&parms);

    pthread_exit(NULL);
}



void *processing(void*)
{
sleep(3); // Ensure that the capture thread has time to initialize and fill buffer
while(1)
{

    /// TODO:  Include some kind of protection to make sure we dont' repeatedly process the same frame
    pthread_mutex_lock(&framelock_mutex);
    // Decode JPEG image stored in the most recently dequeued buffer
    v4l2_process_image(frame, user_buffer.ptr[user_buffer.buf_last]);
    pthread_mutex_unlock(&framelock_mutex);

    // Extract the red channel and save as gray image
    const int mixCh[]= {2,0};
    mixChannels(&frame,1,&gray,1,mixCh,1);
    //gray = gray.clone();  // (uncomment to force a hard copy of image buffer)

    // Detect feature points
    thresh.set_image(gray);
    thresh.detect_blobs();
    vector<Point2f> imagePoints = thresh.get_points();

    /// Optional dilation
    cv::Mat kernel(5,5,CV_8UC1,Scalar(0));
    cv::circle(kernel,Point(2,2),2,Scalar(255));
    dilate(binary,binary,kernel);

    findContours(binary,contours,CV_RETR_EXTERNAL,CV_CHAIN_APPROX_SIMPLE);

    double fps_cnt=fps.fps();
    printf("\r  FPS:  %2.2f   contours: %4d",fps_cnt,contours.size());
    fflush(stdout);

    //imshow("drawing",gray);
    //waitKey(1);
}
pthread_exit(NULL);
}
