#include "mbed.h"
#include "mbed_rpc.h"

#include "stm32l475e_iot01_accelero.h"

#include "uLCD_4DGL.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"
#include "math.h"
#include "accelerometer_handler.h"

#include "magic_wand_model_data.h"
#include "config.h"
#include <string> 

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"



#define PI 3.14159
 
EventQueue queue(32 * EVENTS_EVENT_SIZE);
//Thread qthread;
//qthread.start(callback(&queue, &EventQueue::dispatch_forever));
//Ticker ticker;
//ticker.attach(queue.event(&blink), 1s);



DigitalIn btn(USER_BUTTON);
BufferedSerial pc(USBTX, USBRX);
DigitalOut led1(LED1);
DigitalOut led2(LED2);
DigitalOut led3(LED3);
uLCD_4DGL uLCD(D1, D0, D2); // serial tx, serial rx, reset pin;

Thread capture_thread;

float ref_x=0;float ref_y=0;float ref_z=0;
int over[5]={0};


Thread t1;
Thread t2;
Thread t_model;

float threshold_angle; 

bool flag=true;

struct Config config={64,{20, 10,250}};


const char* host = "192.168.160.16";

volatile int message_num = 0;
volatile int arrivedcount = 0;
volatile bool closed = false;

const char* topic = "Mbed";

WiFiInterface *wifi = WiFiInterface::get_default_instance();
NetworkInterface* net = wifi;
MQTTNetwork mqttNetwork(net);
MQTT::Client<MQTTNetwork, Countdown> client(mqttNetwork);

int gesture_id=0;

/////////////////////////////////////tensorflow/////////////////////////////////////////////////////

// Create an area of memory to use for input, output, and intermediate arrays.
// The size of this will depend on the model you're using, and may need to be
// determined by experimentation.
constexpr int kTensorArenaSize = 60 * 1024;
uint8_t tensor_arena[kTensorArenaSize];

// Return the result of the last prediction
int PredictGesture(float* output) {
  // How many times the most recent gesture has been matched in a row
  static int continuous_count = 0;
  // The result of the last prediction
  static int last_predict = -1;

  // Find whichever output has a probability > 0.8 (they sum to 1)
  int this_predict = -1;
  for (int i = 0; i < label_num; i++) {
    if (output[i] > 0.8) this_predict = i;
  }

  // No gesture was detected above the threshold
  if (this_predict == -1) {
    continuous_count = 0;
    last_predict = label_num;
    return label_num;
  }

  if (last_predict == this_predict) {
    continuous_count += 1;
  } else {
    continuous_count = 0;
  }
  last_predict = this_predict;

  // If we haven't yet had enough consecutive matches for this gesture,
  // report a negative result
  if (continuous_count < config.consecutiveInferenceThresholds[this_predict]) {
    return label_num;
  }
  // Otherwise, we've seen a positive result, so clear all our variables
  // and report it
  continuous_count = 0;
  last_predict = -1;

  return this_predict;
}


int model_run(){


  // Whether we should clear the buffer next time we fetch data
  bool should_clear_buffer = false;
  bool got_data = false;

  // The gesture index of the prediction
  int gesture_index;

  // Set up logging.
  static tflite::MicroErrorReporter micro_error_reporter;
  tflite::ErrorReporter* error_reporter = &micro_error_reporter;

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  const tflite::Model* model = tflite::GetModel(g_magic_wand_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    error_reporter->Report(
        "Model provided is schema version %d not equal "
        "to supported version %d.",
        model->version(), TFLITE_SCHEMA_VERSION);
    return -1;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  static tflite::MicroOpResolver<6> micro_op_resolver;
  micro_op_resolver.AddBuiltin(
      tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
      tflite::ops::micro::Register_DEPTHWISE_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_MAX_POOL_2D,
                               tflite::ops::micro::Register_MAX_POOL_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_CONV_2D,
                               tflite::ops::micro::Register_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_FULLY_CONNECTED,
                               tflite::ops::micro::Register_FULLY_CONNECTED());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_SOFTMAX,
                               tflite::ops::micro::Register_SOFTMAX());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_RESHAPE,
                               tflite::ops::micro::Register_RESHAPE(), 1);

  // Build an interpreter to run the model with
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
  tflite::MicroInterpreter* interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors
  interpreter->AllocateTensors();

  // Obtain pointer to the model's input tensor
  TfLiteTensor* model_input = interpreter->input(0);
  if ((model_input->dims->size != 4) || (model_input->dims->data[0] != 1) ||
      (model_input->dims->data[1] != config.seq_length) ||
      (model_input->dims->data[2] != kChannelNumber) ||
      (model_input->type != kTfLiteFloat32)) {
    error_reporter->Report("Bad input tensor parameters in model");
    return -1;
  }

  int input_length = model_input->bytes / sizeof(float);

  TfLiteStatus setup_status = SetupAccelerometer(error_reporter);
  if (setup_status != kTfLiteOk) {
    error_reporter->Report("Set up failed\n");
    return -1;
  }

  error_reporter->Report("Set up successful...\n");

  while (true) {

    // Attempt to read new data from the accelerometer
    got_data = ReadAccelerometer(error_reporter, model_input->data.f,
                                 input_length, should_clear_buffer);

    // If there was no new data,
    // don't try to clear the buffer again and wait until next time
    if (!got_data) {
      should_clear_buffer = false;
      continue;
    }

    // Run inference, and report any error
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
      error_reporter->Report("Invoke failed on index: %d\n", begin_index);
      continue;
    }

    // Analyze the results to obtain a prediction
    gesture_index = PredictGesture(interpreter->output(0)->data.f);

    // Clear the buffer next time we read data
    should_clear_buffer = gesture_index < label_num;

   
    // Produce an output
    if (gesture_index < label_num) {
      //error_reporter->Report(config.output_message[gesture_index]);
       gesture_id=gesture_index;
    }
  }

}


//////////////////////////////////tensorflow MODEL//////////////////////////////////////////////////////////////




//////////////////////////////////////MQTT/////////////////////////////////////////

/*
void messageArrived(MQTT::MessageData& md) {
    MQTT::Message &message = md.message;
    char msg[300];
    sprintf(msg, "Message arrived: QoS%d, retained %d, dup %d, packetID %d\r\n", message.qos, message.retained, message.dup, message.id);
    printf(msg);
    ThisThread::sleep_for(1000ms);
    char payload[300];
    sprintf(payload, "Payload %.*s\r\n", message.payloadlen, (char*)message.payload);
    printf(payload);
    ++arrivedcount;
}
*/

//ticker.attach(&publish_message,3s);
void publish_message(MQTT::Client<MQTTNetwork, Countdown>* Client,int ID) {
    message_num++;
    MQTT::Message message;
    char buff[100];
    sprintf(buff, "gesture_id:%d",ID);
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
    message.payload = (void*) buff;
    message.payloadlen = strlen(buff) + 1;
    int rc = Client->publish(topic, message);

    printf("rc:  %d\r\n", rc);
    printf("Publish message: %s\r\n", buff);
}

void publish_message(MQTT::Client<MQTTNetwork, Countdown>* Client,int q,int* over) {
    message_num++;
    MQTT::Message message;
    char buff[100];
    sprintf(buff, "%dth gesture has %d data over 30 degrees \n",q,over[q]);
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
    message.payload = (void*) buff;
    message.payloadlen = strlen(buff) + 1;
    int rc = Client->publish(topic, message);

    printf("rc:  %d\r\n", rc);
    printf("Publish message: %s\r\n", buff);
}

void publish_message(MQTT::Client<MQTTNetwork, Countdown>* Client,float* x,float* y,float* z) {
    
    MQTT::Message message;
    char buff[200];
    for(short a=0;a<15;a++){
      sprintf(buff+strlen(buff),"[%f,%f,%f] \n",x[a],y[a],z[a]);
    }
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
    message.payload = (void*) buff;
    message.payloadlen = strlen(buff) + 1;
    int rc = Client->publish(topic, message);

    printf("rc:  %d\r\n", rc);
    printf("Publish message: %s\r\n", buff);
}

void close_mqtt() {
    closed = true;
}



int connect_to_mqtt(){
    
    if (!wifi) {
            printf("ERROR: No WiFiInterface found.\r\n");
            return -1;
    }


    printf("\nConnecting to %s...\r\n", MBED_CONF_APP_WIFI_SSID);
    int ret = wifi->connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD, NSAPI_SECURITY_WPA_WPA2);
    if (ret != 0) {
            printf("\nConnection error: %d\r\n", ret);
            return -1;
    }


    printf("Connecting to TCP network...\r\n");

    SocketAddress sockAddr;
    sockAddr.set_ip_address(host);
    sockAddr.set_port(1883);

    printf("address is %s/%d\r\n", (sockAddr.get_ip_address() ? sockAddr.get_ip_address() : "None"),  (sockAddr.get_port() ? sockAddr.get_port() : 0) ); //check setting
//(host, 1883);//connect to mqtt broker
    int rc = mqttNetwork.connect(sockAddr);
    if (rc != 0) {
            printf("Connection error.");
            return -1;
    }
    printf("Successfully connected!\r\n");

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    data.clientID.cstring = "Mbed";

    if ((rc = client.connect(data)) != 0){
            printf("Fail to connect MQTT\r\n");
    }
    int num = 0;
    while (num != 5) {
            client.yield(100);
            ++num;
    }

    while (1) {
            if (closed) break;
            client.yield(500);
            ThisThread::sleep_for(500ms);
    }

    printf("Ready to close MQTT Network......\n");

    if ((rc = client.unsubscribe(topic)) != 0) {
            printf("Failed: rc from unsubscribe was %d\n", rc);
    }
    if ((rc = client.disconnect()) != 0) {
    printf("Failed: rc from disconnect was %d\n", rc);
    }

    mqttNetwork.disconnect();
    printf("Successfully closed!\n");
    

}

////////////////////////////////////////mqtt/////////////

///////////////////////RPCfunction//////////////////


void stop_capture(Arguments *in, Reply *out);
void accelerator_capture_mode(Arguments *in, Reply *out);


RPCFunction stop(&stop_capture, "stop_capture");
RPCFunction capture(&accelerator_capture_mode,"accelerator_capture_mode");





int main(){

Thread tt;

tt.start(connect_to_mqtt);

    char buf[256], outbuf[256];


    FILE *devin = fdopen(&pc, "r");

    FILE *devout = fdopen(&pc, "w");

    while(1) {

    memset(buf, 0, 256);

    for (int i = 0; ; i++) {

        char recv = fgetc(devin);

        if (recv == '\n') {

            printf("\r\n");

            break;

        }

        buf[i] = fputc(recv, devout);

    }

    //Call the static call method on the RPC class

    RPC::call(buf, outbuf);

    printf("%s\r\n", outbuf);

   }

}




void blink(){
    led2=0;
    led1=1;
    ThisThread::sleep_for(500ms);
    led1=0;
    led3=1;
    ThisThread::sleep_for(500ms);
    led3=0;
    led2=1; //led2=1  ------------------->function is working right now
}


int b=1;
void callref(){
  printf("measuring reference vector...\n");
}

void callgesture(){
  printf("%dth gesture ,pls gesture!\n",b);
  b++;
} 
void ulcdprint(){
    uLCD.cls();
    uLCD.printf("ID:%d",gesture_id);
}

//////////////////////////rpc/////////////////////////

void capture_function(){
  int16_t pDataXYZ[3] = {0};
  queue.call(&callref);
  ThisThread::sleep_for(1s);
  BSP_ACCELERO_AccGetXYZ(pDataXYZ);
  ref_x=pDataXYZ[0];ref_y=pDataXYZ[1];ref_z=pDataXYZ[2];
  blink();

for(short q=0;q<5;q++){

  float x[15],y[15],z[15]; //we collect 15 data(vector) each gesture
  int i=0;
  queue.call(&callgesture);
  ThisThread::sleep_for(1s);
  while(i<15){
    BSP_ACCELERO_AccGetXYZ(pDataXYZ);
    x[i]=pDataXYZ[0];
    y[i]=pDataXYZ[1];
    z[i]=pDataXYZ[2];

    float angle_detect=(x[i]*ref_x+y[i]*ref_y+z[i]*ref_z)/(sqrt(pow(ref_x,2)+pow(ref_y,2)+pow(ref_z,2))*sqrt(pow(x[i],2)+pow(y[i],2)+pow(z[i],2)));
    angle_detect=atan((sqrt(1-(angle_detect*angle_detect))/angle_detect))*180/PI;
    
    if(angle_detect>30){
      over[q]++;
    }
    i++;
    ThisThread::sleep_for(700ms);
  }

    queue.call(&ulcdprint);
    publish_message(&client,gesture_id); //publish the gesture id
    publish_message(&client,x,y,z); //publish the gesture data
  
}

  for(short q=0;q<5;q++){
    publish_message(&client,q,over); //publish the number of data of each gesture over 30 degrees
  }

}


void accelerator_capture_mode(Arguments *in, Reply *out){
  t_model.start(model_run);
  capture_thread.start(capture_function);
}

void stop_capture(Arguments *in, Reply *out){
  capture_thread.join();
}