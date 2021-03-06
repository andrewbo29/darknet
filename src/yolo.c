#include "network.h"
#include "detection_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"

#ifdef OPENCV
#include "opencv2/highgui/highgui_c.h"
#endif

#include <dirent.h>

//char *voc_names[] = {"aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "diningtable", "dog", "horse", "motorbike", "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor"};
//image voc_labels[20];

char *voc_names[] = {"face"};
image voc_labels[1];

void train_yolo(char *cfgfile, char *weightfile)
{
//    char *train_images = "/data/voc/train.txt";
//    char *backup_directory = "/home/pjreddie/backup/";

   char *train_images = "/media/datab/bases/aflw/train.txt";
    // char *train_images = "/media/datab/bases/faces_base/train.txt";
   
//    char *backup_directory = "/media/datac/andrew_workspace/darknet_backup";
   char *backup_directory = "/home/boyarov/darknet_backup";

    char *log_filename = "log.txt";

    srand(time(0));
    data_seed = time(0);
    char *base = basecfg(cfgfile);
    printf("%s\n", base);
    float avg_loss = -1;
    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    printf("Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    int imgs = net.batch*net.subdivisions;
    int i = *net.seen/imgs;
    data train, buffer;


    layer l = net.layers[net.n - 1];

    int side = l.side;
    int classes = l.classes;
    float jitter = l.jitter;

    list *plist = get_paths(train_images);
    //int N = plist->size;
    char **paths = (char **)list_to_array(plist);

    load_args args = {0};
    args.w = net.w;
    args.h = net.h;
    args.paths = paths;
    args.n = imgs;
    args.m = plist->size;
    args.classes = classes;
    args.jitter = jitter;
    args.num_boxes = side;
    args.d = &buffer;
    args.type = REGION_DATA;

    pthread_t load_thread = load_data_in_thread(args);
    clock_t time;
    //while(i*imgs < N*120){

    FILE *log_file = fopen(log_filename, "w");

    while(get_current_batch(net) < net.max_batches){
        i += 1;
        time=clock();
        pthread_join(load_thread, 0);
        train = buffer;
        load_thread = load_data_in_thread(args);

        printf("Loaded: %lf seconds\n", sec(clock()-time));

        time=clock();
        float loss = train_network(net, train);
        if (avg_loss < 0) avg_loss = loss;
        avg_loss = avg_loss*.9 + loss*.1;

        printf("%d: %f, %f avg, %f rate, %lf seconds, %d images\n", i, loss, avg_loss, get_current_rate(net), sec(clock()-time), i*imgs);
        fprintf(log_file, "%d: %f\n", i, loss);
        if(i%1000==0 || i == 600){
            char buff[256];
            sprintf(buff, "%s/%s_%d.weights", backup_directory, base, i);
            save_weights(net, buff);
        }
        free_data(train);
    }

    fclose(log_file);

    char buff[256];
    sprintf(buff, "%s/%s_final.weights", backup_directory, base);
    save_weights(net, buff);
}

void convert_yolo_detections(float *predictions, int classes, int num, int square, int side, int w, int h, float thresh, float **probs, box *boxes, int only_objectness)
{
    int i,j,n;
    //int per_cell = 5*num+classes;
    for (i = 0; i < side*side; ++i){
        int row = i / side;
        int col = i % side;
        for(n = 0; n < num; ++n){
            int index = i*num + n;
            int p_index = side*side*classes + i*num + n;
            float scale = predictions[p_index];
            int box_index = side*side*(classes + num) + (i*num + n)*4;
            boxes[index].x = (predictions[box_index + 0] + col) / side * w;
            boxes[index].y = (predictions[box_index + 1] + row) / side * h;
            boxes[index].w = pow(predictions[box_index + 2], (square?2:1)) * w;
            boxes[index].h = pow(predictions[box_index + 3], (square?2:1)) * h;
            for(j = 0; j < classes; ++j){
                int class_index = i*classes;
                float prob = scale*predictions[class_index+j];
                probs[index][j] = (prob > thresh) ? prob : 0;
            }
            if(only_objectness){
                probs[index][0] = scale;
            }
        }
    }
}

void print_yolo_detections(FILE **fps, char *id, box *boxes, float **probs, int total, int classes, int w, int h)
{
    int i, j;
    for(i = 0; i < total; ++i){
        float xmin = boxes[i].x - boxes[i].w/2.;
        float xmax = boxes[i].x + boxes[i].w/2.;
        float ymin = boxes[i].y - boxes[i].h/2.;
        float ymax = boxes[i].y + boxes[i].h/2.;

        if (xmin < 0) xmin = 0;
        if (ymin < 0) ymin = 0;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        for(j = 0; j < classes; ++j){
            if (probs[i][j]) fprintf(fps[j], "%s %f %f %f %f %f\n", id, probs[i][j],
                    xmin, ymin, xmax, ymax);
        }
    }
}

void validate_yolo(char *cfgfile, char *weightfile)
{
    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    srand(time(0));

    char *base = "results/comp4_det_test_";
    list *plist = get_paths("data/voc.2007.test");
    //list *plist = get_paths("data/voc.2012.test");
    char **paths = (char **)list_to_array(plist);

    layer l = net.layers[net.n-1];
    int classes = l.classes;
    int square = l.sqrt;
    int side = l.side;

    int j;
    FILE **fps = calloc(classes, sizeof(FILE *));
    for(j = 0; j < classes; ++j){
        char buff[1024];
        snprintf(buff, 1024, "%s%s.txt", base, voc_names[j]);
        fps[j] = fopen(buff, "w");
    }
    box *boxes = calloc(side*side*l.n, sizeof(box));
    float **probs = calloc(side*side*l.n, sizeof(float *));
    for(j = 0; j < side*side*l.n; ++j) probs[j] = calloc(classes, sizeof(float *));

    int m = plist->size;
    int i=0;
    int t;

    float thresh = .001;
    int nms = 1;
    float iou_thresh = .5;

    int nthreads = 2;
    image *val = calloc(nthreads, sizeof(image));
    image *val_resized = calloc(nthreads, sizeof(image));
    image *buf = calloc(nthreads, sizeof(image));
    image *buf_resized = calloc(nthreads, sizeof(image));
    pthread_t *thr = calloc(nthreads, sizeof(pthread_t));

    load_args args = {0};
    args.w = net.w;
    args.h = net.h;
    args.type = IMAGE_DATA;

    for(t = 0; t < nthreads; ++t){
        args.path = paths[i+t];
        args.im = &buf[t];
        args.resized = &buf_resized[t];
        thr[t] = load_data_in_thread(args);
    }
    time_t start = time(0);
    for(i = nthreads; i < m+nthreads; i += nthreads){
        fprintf(stderr, "%d\n", i);
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            pthread_join(thr[t], 0);
            val[t] = buf[t];
            val_resized[t] = buf_resized[t];
        }
        for(t = 0; t < nthreads && i+t < m; ++t){
            args.path = paths[i+t];
            args.im = &buf[t];
            args.resized = &buf_resized[t];
            thr[t] = load_data_in_thread(args);
        }
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            char *path = paths[i+t-nthreads];
            char *id = basecfg(path);
            float *X = val_resized[t].data;
            float *predictions = network_predict(net, X);
            int w = val[t].w;
            int h = val[t].h;
            convert_yolo_detections(predictions, classes, l.n, square, side, w, h, thresh, probs, boxes, 0);
            if (nms) do_nms_sort(boxes, probs, side*side*l.n, classes, iou_thresh);
            print_yolo_detections(fps, id, boxes, probs, side*side*l.n, classes, w, h);
            free(id);
            free_image(val[t]);
            free_image(val_resized[t]);
        }
    }
    fprintf(stderr, "Total Detection Time: %f Seconds\n", (double)(time(0) - start));
}

void validate_yolo_recall(char *cfgfile, char *weightfile)
{
    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    srand(time(0));

    char *base = "results/comp4_det_test_";
    list *plist = get_paths("data/voc.2007.test");
    char **paths = (char **)list_to_array(plist);

    layer l = net.layers[net.n-1];
    int classes = l.classes;
    int square = l.sqrt;
    int side = l.side;

    int j, k;
    FILE **fps = calloc(classes, sizeof(FILE *));
    for(j = 0; j < classes; ++j){
        char buff[1024];
        snprintf(buff, 1024, "%s%s.txt", base, voc_names[j]);
        fps[j] = fopen(buff, "w");
    }
    box *boxes = calloc(side*side*l.n, sizeof(box));
    float **probs = calloc(side*side*l.n, sizeof(float *));
    for(j = 0; j < side*side*l.n; ++j) probs[j] = calloc(classes, sizeof(float *));

    int m = plist->size;
    int i=0;

    float thresh = .001;
    float iou_thresh = .5;
    float nms = 0;

    int total = 0;
    int correct = 0;
    int proposals = 0;
    float avg_iou = 0;

    for(i = 0; i < m; ++i){
        char *path = paths[i];
        image orig = load_image_color(path, 0, 0);
        image sized = resize_image(orig, net.w, net.h);
        char *id = basecfg(path);
        float *predictions = network_predict(net, sized.data);
        convert_yolo_detections(predictions, classes, l.n, square, side, 1, 1, thresh, probs, boxes, 1);
        if (nms) do_nms(boxes, probs, side*side*l.n, 1, nms);

        char *labelpath = find_replace(path, "images", "labels");
        labelpath = find_replace(labelpath, "JPEGImages", "labels");
        labelpath = find_replace(labelpath, ".jpg", ".txt");
        labelpath = find_replace(labelpath, ".JPEG", ".txt");

        int num_labels = 0;
        box_label *truth = read_boxes(labelpath, &num_labels);
        for(k = 0; k < side*side*l.n; ++k){
            if(probs[k][0] > thresh){
                ++proposals;
            }
        }
        for (j = 0; j < num_labels; ++j) {
            ++total;
            box t = {truth[j].x, truth[j].y, truth[j].w, truth[j].h};
            float best_iou = 0;
            for(k = 0; k < side*side*l.n; ++k){
                float iou = box_iou(boxes[k], t);
                if(probs[k][0] > thresh && iou > best_iou){
                    best_iou = iou;
                }
            }
            avg_iou += best_iou;
            if(best_iou > iou_thresh){
                ++correct;
            }
        }

        fprintf(stderr, "%5d %5d %5d\tRPs/Img: %.2f\tIOU: %.2f%%\tRecall:%.2f%%\n", i, correct, total, (float)proposals/(i+1), avg_iou*100/total, 100.*correct/total);
        free(id);
        free_image(orig);
        free_image(sized);
    }
}

void test_yolo(char *cfgfile, char *weightfile, char *filename, float thresh)
{

    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    detection_layer l = net.layers[net.n-1];
    set_batch_network(&net, 1);
    srand(2222222);
        clock_t time;
    char buff[256];
    char *input = buff;
    int j;
    float nms=.5;
    box *boxes = calloc(l.side*l.side*l.n, sizeof(box));
    float **probs = calloc(l.side*l.side*l.n, sizeof(float *));
    for(j = 0; j < l.side*l.side*l.n; ++j) probs[j] = calloc(l.classes, sizeof(float *));
    while(1){
        if(filename){
            strncpy(input, filename, 256);
        } else {
            printf("Enter Image Path: ");
            fflush(stdout);
            input = fgets(input, 256, stdin);
            if(!input) return;
            strtok(input, "\n");
        }
        image im = load_image_color(input,0,0);
        image sized = resize_image(im, net.w, net.h);
        float *X = sized.data;
        time=clock();
        float *predictions = network_predict(net, X);
        printf("%s: Predicted in %f seconds.\n", input, sec(clock()-time));
        convert_yolo_detections(predictions, l.classes, l.n, l.sqrt, l.side, 1, 1, thresh, probs, boxes, 0);
        if (nms) do_nms_sort(boxes, probs, l.side*l.side*l.n, l.classes, nms);
//        draw_detections(im, l.side*l.side*l.n, thresh, boxes, probs, voc_names, voc_labels, 20);
        draw_detections(im, l.side*l.side*l.n, thresh, boxes, probs, voc_names, voc_labels, 1);

        show_image(im, "predictions");

//        show_image(sized, "resized");
        free_image(im);
//        free_image(sized);

#ifdef OPENCV
        cvWaitKey(0);
        cvDestroyAllWindows();
#endif
        if (filename) break;
    }
}

/*
#ifdef OPENCV
image ipl_to_image(IplImage* src);
#include "opencv2/highgui/highgui_c.h"
#include "opencv2/imgproc/imgproc_c.h"

void demo_swag(char *cfgfile, char *weightfile, float thresh)
{
network net = parse_network_cfg(cfgfile);
if(weightfile){
load_weights(&net, weightfile);
}
detection_layer layer = net.layers[net.n-1];
CvCapture *capture = cvCaptureFromCAM(-1);
set_batch_network(&net, 1);
srand(2222222);
while(1){
IplImage* frame = cvQueryFrame(capture);
image im = ipl_to_image(frame);
cvReleaseImage(&frame);
rgbgr_image(im);

image sized = resize_image(im, net.w, net.h);
float *X = sized.data;
float *predictions = network_predict(net, X);
draw_swag(im, predictions, layer.side, layer.n, "predictions", thresh);
free_image(im);
free_image(sized);
cvWaitKey(10);
}
}
#else
void demo_swag(char *cfgfile, char *weightfile, float thresh){}
#endif
 */

void demo_yolo(char *cfgfile, char *weightfile, float thresh, int cam_index);
#ifndef GPU
void demo_yolo(char *cfgfile, char *weightfile, float thresh, int cam_index)
{
    fprintf(stderr, "Darknet must be compiled with CUDA for YOLO demo.\n");
}
#endif

void run_yolo(int argc, char **argv)
{
    int i;
//    for(i = 0; i < 20; ++i){
    for(i = 0; i < 1; ++i){
        char buff[256];
        sprintf(buff, "data/labels/%s.png", voc_names[i]);
        voc_labels[i] = load_image_color(buff, 0, 0);
    }

    float thresh = find_float_arg(argc, argv, "-thresh", .2);
    int cam_index = find_int_arg(argc, argv, "-c", 0);
    if(argc < 4){
        fprintf(stderr, "usage: %s %s [train/test/valid] [cfg] [weights (optional)]\n", argv[0], argv[1]);
        return;
    }

    if (0==strcmp(argv[2], "validate")) {
        char *cfg_detect = argv[3];
        char *weights_detect = argv[4];
        char *cfg_validate = argv[5];
        char *weights_validate = argv[6];
        char *filename = argv[7];
        detect_validate(cfg_detect, weights_detect, cfg_validate, weights_validate, filename, thresh);
    }
    else if (0 == strcmp(argv[2], "predict_file")) {
        char *cfg = argv[3];
        char *weights = argv[4];
        char *filename = argv[5];
        char *res_dir = argv[6];
        predict_file(cfg, weights, filename, res_dir, thresh);
    }
    else {
        char *cfg = argv[3];
        char *weights = (argc > 4) ? argv[4] : 0;
        char *filename = (argc > 5) ? argv[5] : 0;
        char *resFilename = (argc > 6) ? argv[6] : 0;
        if (0 == strcmp(argv[2], "test")) test_yolo(cfg, weights, filename, thresh);
        else if (0 == strcmp(argv[2], "train")) train_yolo(cfg, weights);
        else if (0 == strcmp(argv[2], "valid")) validate_yolo(cfg, weights);
        else if (0 == strcmp(argv[2], "recall")) validate_yolo_recall(cfg, weights);
        else if (0 == strcmp(argv[2], "demo")) demo_yolo(cfg, weights, thresh, cam_index);
        else if (0 == strcmp(argv[2], "print")) print_yolo(cfg, weights, filename, resFilename, thresh);
        else if (0 == strcmp(argv[2], "folder")) folder_yolo(cfg, weights, filename, thresh);
        else if (0 == strcmp(argv[2], "time")) time_yolo(cfg, weights, filename, thresh);
    }
}

void print_yolo(char *cfgfile, char *weightfile, char *dirName, char *resDir, float thresh)
{
    network net = parse_network_cfg(cfgfile);
    if (weightfile) {
        load_weights(&net, weightfile);
    }

    char *ext = "-out.txt";

    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(dirName)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char * fname = ent->d_name;
            if (strlen(fname) > 5) {
                char *fileFold = malloc(strlen(dirName) + strlen(fname) + 1);
                strcpy(fileFold, dirName);
                strcat(fileFold, fname);

                char *fnameRes = fname + 5;
                char *name = malloc(strlen(resDir) + strlen(fnameRes) + 1);
                strcpy(name, resDir);
                strcat(name, fnameRes);
                char *lastdot = strrchr(name, '.');
                if (lastdot != NULL) {
                    *lastdot = '\0';
                }
                char *resFilename = malloc(strlen(name) + strlen(ext) + 1);
                strcpy(resFilename, name);
                strcat(resFilename, ext);

                print_process_file(resFilename, fileFold, net, thresh);
            }
        }
        closedir(dir);
    }
}

void print_process_file(char *resFilename, char *fileFold, network net, float thresh)
{
    FILE *f = fopen(resFilename, "w");

    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    size_t read;

    fp = fopen(fileFold, "r");

    char *path = "/media/datab/bases/FDDB/";
    char *ext = ".jpg";
    while ((read = getline(&line, &len, fp)) != -1) {
        line[strlen(line) - 1] = 0;
        char *filename = malloc(strlen(path) + strlen(line) + strlen(ext) + 1);
        strcpy(filename, path);
        strcat(filename, line);
        strcat(filename, ext);

        print_yolo_file(f, filename, line, net, thresh);
    }
    fclose(fp);
    if (line)
        free(line);

    fclose(f);
}

void print_yolo_file(FILE *f, char *filename, char *shortFilename, network net, float thresh)
{
    detection_layer l = net.layers[net.n - 1];
    set_batch_network(&net, 1);
    srand(2222222);
    clock_t time;
    char buff[256];
    char *input = buff;
    int j;
    float nms = .5;
    box *boxes = calloc(l.side * l.side * l.n, sizeof(box));
    float **probs = calloc(l.side * l.side * l.n, sizeof(float *));
    for (j = 0; j < l.side * l.side * l.n; ++j) probs[j] = calloc(l.classes, sizeof(float *));
    strncpy(input, filename, 256);
    image im = load_image_color(input, 0, 0);
    image sized = resize_image(im, net.w, net.h);
    float *X = sized.data;
    time = clock();
    float *predictions = network_predict(net, X);
    printf("%s: Predicted in %f seconds.\n", input, sec(clock() - time));
    convert_yolo_detections(predictions, l.classes, l.n, l.sqrt, l.side, 1, 1, thresh, probs, boxes, 0);
    if (nms) do_nms_sort(boxes, probs, l.side * l.side * l.n, l.classes, nms);

    free_image(im);
    free_image(sized);

    int total = l.side*l.side*l.n;

    double predThresh = .05;

    int objNum = 0;
    int k;
    for(k = 0; k < total; ++k){
        if (probs[k][0] > predThresh) {
            objNum++;
        }
    }

    fprintf(f, "%s\n%d\n", shortFilename, objNum);

    int i;
    for(i = 0; i < total; ++i){
        if (probs[i][0] > predThresh) {
            float xmin = (boxes[i].x - boxes[i].w/2.)*im.w;
            float xmax = (boxes[i].x + boxes[i].w/2.)*im.w;
            float ymin = (boxes[i].y - boxes[i].h/2.)*im.h;
            float ymax = (boxes[i].y + boxes[i].h/2.)*im.h;

            if (xmin < 0) xmin = 0;
            if (ymin < 0) ymin = 0;
            if (xmax > im.w) xmax = im.w;
            if (ymax > im.h) ymax = im.h;

            int x = (int)(xmin);
            int y = (int)(ymin);
            int w = (int)((xmax - xmin));
            int h = (int)((ymax - ymin));

            fprintf(f, "%d %d %d %d %f\n", x, y, w, h, probs[i][0]);
        }
    }
}

void folder_yolo(char *cfgfile, char *weightfile, char *dirName, float thresh)
{
    network net = parse_network_cfg(cfgfile);
    if (weightfile) {
        load_weights(&net, weightfile);
    }

    char *jpg = "jpg";

    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(dirName)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char *fname = ent->d_name;
            if (strlen(fname) > 3) {
                char *dot = strrchr(fname, '.');
                char *ext = dot + 1;

                if (strcmp(ext, jpg) == 0) {
                    char *filename = malloc(strlen(dirName) + strlen(fname) + 1);
                    strcpy(filename, dirName);
                    strcat(filename, fname);

                    folder_yolo_file(filename, net, thresh);
                }
            }
        }
        closedir(dir);
    }
}

void folder_yolo_file(char *filename, network net, float thresh)
{
    detection_layer l = net.layers[net.n - 1];
    set_batch_network(&net, 1);
    srand(2222222);
    clock_t time;
    char buff[256];
    char *input = buff;
    int j;
    float nms = .5;
    box *boxes = calloc(l.side * l.side * l.n, sizeof(box));
    float **probs = calloc(l.side * l.side * l.n, sizeof(float *));
    for (j = 0; j < l.side * l.side * l.n; ++j) probs[j] = calloc(l.classes, sizeof(float *));
    strncpy(input, filename, 256);
    image im = load_image_color(input, 0, 0);
    image sized = resize_image(im, net.w, net.h);
    float *X = sized.data;
    time = clock();
    float *predictions = network_predict(net, X);
    printf("%s: Predicted in %f seconds.\n", input, sec(clock() - time));
    convert_yolo_detections(predictions, l.classes, l.n, l.sqrt, l.side, 1, 1, thresh, probs, boxes, 0);
    if (nms) do_nms_sort(boxes, probs, l.side * l.side * l.n, l.classes, nms);

    free_image(im);
    free_image(sized);

    int total = l.side*l.side*l.n;

    double predThresh = .05;

    int i;
    int num = 0;
    for(i = 0; i < total; ++i){
        if (probs[i][0] > predThresh) {
            char ext[20];
            sprintf(ext, "_yolo_%d.face", num);
            ++num;

            char *resFilename = malloc(strlen(filename) + strlen(ext) + 1);
            strcpy(resFilename, filename);
            strcat(resFilename, ext);

            FILE *f = fopen(resFilename, "w");

            float xmin = (boxes[i].x - boxes[i].w/2.)*im.w;
            float xmax = (boxes[i].x + boxes[i].w/2.)*im.w;
            float ymin = (boxes[i].y - boxes[i].h/2.)*im.h;
            float ymax = (boxes[i].y + boxes[i].h/2.)*im.h;

            if (xmin < 0) xmin = 0;
            if (ymin < 0) ymin = 0;
            if (xmax > im.w) xmax = im.w;
            if (ymax > im.h) ymax = im.h;

            int x = (int)(xmin);
            int y = (int)(ymin);
            int w = (int)((xmax - xmin));
            int h = (int)((ymax - ymin));

            fprintf(f, "%d\t%d\t%d\t%d\n", x, y, w, h);

            fclose(f);
        }
    }
}

void time_yolo(char *cfgfile, char *weightfile, char *dirName, float thresh)
{
    network net = parse_network_cfg(cfgfile);
    if (weightfile) {
        load_weights(&net, weightfile);
    }

    char *jpg = "jpg";

    float sumTime = 0.;
    int numTime = 0;

    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(dirName)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char *fname = ent->d_name;
            if (strlen(fname) > 3) {
                char *dot = strrchr(fname, '.');
                char *ext = dot + 1;

                if (strcmp(ext, jpg) == 0) {
                    char *filename = malloc(strlen(dirName) + strlen(fname) + 1);
                    strcpy(filename, dirName);
                    strcat(filename, fname);

                    clock_t time;
                    time = clock();
                    time_yolo_file(filename, net, thresh);
                    sumTime += sec(clock() - time);
                    printf("%s: %f\n", filename, sec(clock() - time));
                    numTime += 1;
                }
            }
        }
        closedir(dir);
    }

    printf("Mean time: %f\n", sumTime / numTime);
}

void time_yolo_file(char *filename, network net, float thresh)
{
    detection_layer l = net.layers[net.n - 1];
    set_batch_network(&net, 1);
    srand(2222222);
    char buff[256];
    char *input = buff;
    int j;
    float nms = .5;
    box *boxes = calloc(l.side * l.side * l.n, sizeof(box));
    float **probs = calloc(l.side * l.side * l.n, sizeof(float *));
    for (j = 0; j < l.side * l.side * l.n; ++j) probs[j] = calloc(l.classes, sizeof(float *));
    strncpy(input, filename, 256);
    image im = load_image_color(input, 0, 0);
    image sized = resize_image(im, net.w, net.h);
    float *X = sized.data;
    float *predictions = network_predict(net, X);
    convert_yolo_detections(predictions, l.classes, l.n, l.sqrt, l.side, 1, 1, thresh, probs, boxes, 0);
    if (nms) do_nms_sort(boxes, probs, l.side * l.side * l.n, l.classes, nms);

    free_image(im);
    free_image(sized);
}

void detect_validate(char *cfgfile_detect, char *weightfile_detect, char *cfgfile_validate, char *weightfile_validate, char *filename, float thresh)
{
    network detect_net = parse_network_cfg(cfgfile_detect);
    if(weightfile_detect){
        load_weights(&detect_net, weightfile_detect);
    }

    network validate_net = parse_network_cfg(cfgfile_validate);
    if(weightfile_validate){
        load_weights(&validate_net, weightfile_validate);
    }

    detection_layer l = detect_net.layers[detect_net.n - 1];
    set_batch_network(&detect_net, 1);
    srand(2222222);
    clock_t time;
    char buff[256];
    char *input = buff;
    int j;
    float nms = .5;
    box *boxes = calloc(l.side * l.side * l.n, sizeof(box));
    float **probs = calloc(l.side * l.side * l.n, sizeof(float *));
    for (j = 0; j < l.side * l.side * l.n; ++j) probs[j] = calloc(l.classes, sizeof(float *));
    strncpy(input, filename, 256);
    image im = load_image_color(input, 0, 0);
    image sized = resize_image(im, detect_net.w, detect_net.h);
    float *X = sized.data;
    time = clock();
    float *predictions = network_predict(detect_net, X);
    convert_yolo_detections(predictions, l.classes, l.n, l.sqrt, l.side, 1, 1, thresh, probs, boxes, 0);
    if (nms) do_nms_sort(boxes, probs, l.side * l.side * l.n, l.classes, nms);

    int total = l.side*l.side*l.n;

    double predThresh = 0;

    box *valid_boxes = calloc(l.side*l.side*l.n, sizeof(box));
    float **valid_probs = calloc(l.side * l.side * l.n, sizeof(float *));
    int index = 0;
    int proposals_number = 0;
    int i;
    for(i = 0; i < total; ++i) {
        if (probs[i][0] > predThresh) {
            float xmin = (boxes[i].x - boxes[i].w / 2.) * im.w;
            float xmax = (boxes[i].x + boxes[i].w / 2.) * im.w;
            float ymin = (boxes[i].y - boxes[i].h / 2.) * im.h;
            float ymax = (boxes[i].y + boxes[i].h / 2.) * im.h;

            if (xmin < 0) xmin = 0;
            if (ymin < 0) ymin = 0;
            if (xmax > im.w) xmax = im.w;
            if (ymax > im.h) ymax = im.h;

            float x = xmin;
            float y = ymin;
            float w = xmax - xmin;
            float h = ymax - ymin;

            image proposal = crop_image(im, x, y, w, h);
            ++proposals_number;
            if (validate_image(validate_net, proposal)) {
                valid_boxes[index] = boxes[i];
                valid_probs[index] = probs[i];
                ++index;
            }
        }
    }

    printf("%s: Predicted in %f seconds.\n", input, sec(clock() - time));
    printf("Proposals number: %d, faces number: %d\n", proposals_number, index);

    draw_detections(im, index, predThresh, valid_boxes, valid_probs, voc_names, voc_labels, 1);
    show_image(im, "predictions");

    free_image(im);
    free_image(sized);

    #ifdef OPENCV
        cvWaitKey(0);
        cvDestroyAllWindows();
    #endif
}

int validate_image(network net, image im)
{
    set_batch_network(&net, 1);
    srand(2222222);

    if (im.w < 1 || im.h < 1 || im.c < 1) {
        return 0;
    }
    image res_im = resize_image(im, net.w, net.h);

    float *X = res_im.data;
    float *predictions = network_predict(net, X);

    float pred_thresh = 0.5;
    if (predictions[1] > pred_thresh) {
        return 1;
    }
    return 0;
}

void predict_file(char *cfgfile, char *weightfile, char *filename, char *res_dir, float thresh)
{
    network net = parse_network_cfg(cfgfile);
    if (weightfile) {
        load_weights(&net, weightfile);
    }

    FILE *f = fopen(filename, "r");

    char * line = NULL;
    size_t len = 0;
    size_t read;

    char *ext = ".txt";
    while ((read = getline(&line, &len, f)) != -1) {
        char buff[256];
        char *image_filename = buff;
        strncpy(image_filename, line, 256);
        image_filename[strlen(image_filename) - 1] = 0;
        char *pch;
        pch = strtok(line, "/");
        int num = 0;
        char buff1[256];
        char *res_filename = buff1;
        strncpy(res_filename, res_dir, 256);
        while (pch != NULL) {
            if (num == 6) {
                char *fname = pch;
                char *dot = strrchr(fname, '.');
                *dot = '\0';
                strcat(res_filename, fname);
                strcat(res_filename, ext);
            }
            num += 1;
            pch = strtok(NULL, "/");
        }

        predict_file_image(image_filename, res_filename, net, thresh);
    }
    if (line)
        free(line);

    fclose(f);
}

void predict_file_image(char *image_filename, char *res_filename, network net, float thresh)
{
    detection_layer l = net.layers[net.n - 1];
    set_batch_network(&net, 1);
    srand(2222222);
    clock_t time;
    char buff[256];
    char *input = buff;
    int j;
    float nms = .5;
    box *boxes = calloc(l.side * l.side * l.n, sizeof(box));
    float **probs = calloc(l.side * l.side * l.n, sizeof(float *));
    for (j = 0; j < l.side * l.side * l.n; ++j) probs[j] = calloc(l.classes, sizeof(float *));
    strncpy(input, image_filename, 256);
    image im = load_image_color(input, 0, 0);
    image sized = resize_image(im, net.w, net.h);
    float *X = sized.data;
    time = clock();
    float *predictions = network_predict(net, X);
    printf("%s: Predicted in %f seconds.\n", input, sec(clock() - time));
    convert_yolo_detections(predictions, l.classes, l.n, l.sqrt, l.side, 1, 1, thresh, probs, boxes, 0);
    if (nms) do_nms_sort(boxes, probs, l.side * l.side * l.n, l.classes, nms);

    free_image(im);
    free_image(sized);

    int total = l.side*l.side*l.n;

    double predThresh = .2;

    FILE *res_f = fopen(res_filename, "w");

    int i;
    for(i = 0; i < total; ++i){
        if (probs[i][0] > predThresh) {
            float xmin = (boxes[i].x - boxes[i].w/2.)*im.w;
            float xmax = (boxes[i].x + boxes[i].w/2.)*im.w;
            float ymin = (boxes[i].y - boxes[i].h/2.)*im.h;
            float ymax = (boxes[i].y + boxes[i].h/2.)*im.h;

            if (xmin < 0) xmin = 0;
            if (ymin < 0) ymin = 0;
            if (xmax > im.w) xmax = im.w;
            if (ymax > im.h) ymax = im.h;

            int x = (int)(xmin);
            int y = (int)(ymin);
            int w = (int)((xmax - xmin));
            int h = (int)((ymax - ymin));

            fprintf(res_f, "%d %d %d %d\n", x, y, w, h);
        }
    }

    fclose(res_f);
}

