import numpy as np
import tensorflow as tf
import os
import datetime
import threading
from common import common as cm
from keras.models import load_model
from tkinter import Tk, StringVar, Label
    
CLASSIFICATION_OUTPUT_TO_STR = {0: "STANDING", 1: "SITTING", 2: "LYING DOWN", 3: "BENDING"}
fallNum = 0

lowest_y_point = 1000

# Threshold of how many meters from the lowest point in the room is acceptable to approve the person is lying down on the ground
M_FROM_FLOOR = 0.25

objects_per_room = {}   

comm = cm()

def importFloorData(roomNumber):
    filepath = "data/floorplans/" + str(roomNumber) + ".txt"
    if (os.path.isfile(filepath)):
        file = open(filepath, 'r')
        objects_per_room[str(roomNumber)] = []  # This room has a list of objects
        objects = file.read().splitlines()
        num_objects = int(len(objects) / 4)  # Each file has 4 coords
        for i in range(num_objects):
            objects_per_room[str(roomNumber)].append(
                objects[(i * 4):(i * 4) + 4])  # Append the object to the list of objects for that particular room
    print("FLOOR OBJECT DATA IMPORTED FOR ROOM #" + str(roomNumber) + "... !")
    return

# deprecated but still usable, isLayingOnTheFloor() is the new implementation
def isWithinGroundRange(x, z, roomNumber):
    objects = objects_per_room[str(roomNumber)]  # Impoted floor data for that room
    for object in objects:
        if (x > float(object[0]) and x < float(object[1]) and z > float(object[2]) and z < float(object[3])):  # If person is on that object
            return False
    return True


def getLSTMClassification(inputVals):
    if (inputVals[0][0] < 0.1):
        return "LYING DOWN"
    global graph
    with graph.as_default():
        classification_output = model.predict(np.array([tuple(inputVals)]).reshape(1,7,1))
    return CLASSIFICATION_OUTPUT_TO_STR[np.argmax(classification_output,1)[0]]

def isLayingOnTheFloor(footRightPosY, footLeftPosY):
    if ((footRightPosY < (lowest_y_point + M_FROM_FLOOR)) and (footLeftPosY < (lowest_y_point + M_FROM_FLOOR))):
        return True
    return False

def getTime():
    return str(datetime.datetime.now().strftime('%H:%M:%S'))

def judgePosture(labelText, lowest_y_point, person=0):
    print("judge posture")
    fileName = 'real_time_joints_data_' + str(person) + '.txt'
    file = open(fileName, 'w+')
    index = 0
    # Initialization step
    # Extract data from sensor and take the lowest point of foot left & right
    while (index < 300):  # 3 sec * 10numbers/frame 10frames/sec
        lines = file.read().splitlines()
        file.seek(0)
        if (len(lines) >= index + 10):  # if there is new data
            index += 10
            inp = lines[index - 10:index]  # get data for next frame
            # Which Y-position is lower?
            if (float(inp[7]) < float(inp[8])):  # Then use inp[5] because it's the smallest Y-point
                if (lowest_y_point > float(inp[7])):
                    lowest_y_point = float(inp[7])
            else:
                if (lowest_y_point > float(inp[8])):
                    lowest_y_point = float(inp[8])

    # print("LOWEST_Y_POINT === " + str(lowest_y_point))
    # End of initialization step

    file = open(fileName, 'w+')
    index = 0

    # Start system
    while True:
        # os.system('cls')
        global posture
        lines = file.read().splitlines()
        file.seek(0)  # move cursor to beggining of file for next loop
        if (len(lines) >= index + 10):  # if there is new data
            index += 10
            inp = lines[index - 10:index]  # get data for next frame
            # index += 20 #10 FPS
            inp = [float(i) for i in inp]
            inputVals = np.random.rand(1, 7)
            inputVals[0] = inp[:7]  # Only the first 7 values. The other two values will be used to check the floor plan
            posture = getLSTMClassification(inputVals)

            labelText.set(getTime() + " " + posture)
            root.update()

            # print(getTime() + " " + posture)
            if (posture == "LYING DOWN"):
                if (isLayingOnTheFloor(float(inp[7]), float(inp[8]))):
                    timestamp = inp[9]
                    fall = True
                    allowed = 2  # at least 95% of the time detected as LYING DOWN.
                    allowed_not_on_floor = 5
                    for i in range(20):  # check LYING DOWN for 2 seconds (10fps*2s = 20 frames)
                        while (len(lines) < index + 10):
                            lines = file.read().splitlines()
                            file.seek(0)  # move cursor to beggining of file for next loop
                        index += 10
                        inp = lines[index - 10:index]  # get data for next frame
                        # index += 20 #10 FPS
                        inp = [float(i) for i in inp]
                        inputVals = np.random.rand(1, 7)
                        inputVals[0] = inp[:7]
                        # timestamps.append(inp[9])
                        posture = getLSTMClassification(inputVals)
                        # print(datetime.datetime.now(), posture)

                        labelText.set(getTime() + " " + posture)
                        root.update()

                        if (posture == "LYING DOWN"):  # Is the person LYING DOWN on the floor?
                            if (isLayingOnTheFloor(float(inp[7]), float(inp[8])) == False):
                                if (allowed_not_on_floor == 0):
                                    print("PERSON IS NOT LAYING ON THE FLOOR! Not fall..!")
                                    fall = False
                                    break
                                else:
                                    allowed_not_on_floor -= 1
                        else:  # 10% allowed to not be LYING DOWN (2/20)
                            if (allowed == 0):
                                print("PERSON HAS NOT BEEN LAYING ON THE FLOOR FOR MORE THAN 2 SECONDS! Not fall..!")
                                fall = False
                                break
                            else:
                                allowed -= 1
                    if (fall):
                        labelText.set(getTime() + " " + "FALLEN!")
                        root.update()
                        # print(getTime() + " " + "--FALLEN!--")

                        # You can now reset index=0 and delete the file to restart the While loop from current data.
                        while posture == "LYING DOWN":  # Fallen until detected in another posture
                            while (len(lines) < index + 10):
                                lines = file.read().splitlines()
                                file.seek(0)  # move cursor to beggining of file for next loop
                            index += 10
                            inp = lines[index - 9:index]  # get data for next frame
                            inp = [float(i) for i in inp]
                            inputVals = np.random.rand(1, 7)
                            inputVals[0] = inp[:7]
                            posture = getLSTMClassification(inputVals)
                            # print(getTime() + " " + posture)
                            if posture != "LYING DOWN":
                                labelText.set(getTime() + " " + posture)
                                root.update()
                                file = open(fileName, 'w+')
                                index = 0
        if (index > 2500):
            # index = 300
            file = open(fileName, 'w+')
            index = 0
    
class postureThread (threading.Thread):
    def __init__(self, threadID, labelText, lowest_y_point):
        threading.Thread.__init__(self)
        self.threadID = threadID
        self.labelText = labelText
        self.lowest_y_point = lowest_y_point

    def run(self):
        print(self.threadID, ' is running')
        judgePosture(self.labelText, self.lowest_y_point, self.threadID)

if __name__ == "__main__":
    print("Loading model..")
    model = load_model('postureDetection_LSTM.h5')
    graph = tf.get_default_graph()

    root = ""
    labelText = ""

    root = Tk()
    root.title("POSTURE DETECTION")
    root.geometry("800x500")
    labelText = [StringVar() for i in range(6)]
    for label in labelText:
        label.set('Starting...!')

    button0 = Label(root, textvariable=labelText[0], font=("Helvetica", 40))
    button1 = Label(root, textvariable=labelText[1], font=("Helvetica", 40))
    button2 = Label(root, textvariable=labelText[2], font=("Helvetica", 40))
    button3 = Label(root, textvariable=labelText[3], font=("Helvetica", 40))
    button4 = Label(root, textvariable=labelText[4], font=("Helvetica", 40))
    button5 = Label(root, textvariable=labelText[5], font=("Helvetica", 40))

    button0.pack()
    button1.pack()
    button2.pack()
    button3.pack()
    button4.pack()
    button5.pack()

    thread = []

    for i in range(6):
        thread.append(postureThread(i, labelText[i], lowest_y_point))
    print(len(thread))

    for t in thread:
        print(t.threadID)
        t.start()
    
    # for t in thread:
    #     t.join()

    root.mainloop()
    # for i in range(6):
    #     judgePosture(labelText[0], lowest_y_point)

    # # Initialization step
    # # Extract data from sensor and take the lowest point of foot left & right
    # while (index < 300):  # 3 sec * 10numbers/frame 10frames/sec
    #     lines = file.read().splitlines()
    #     file.seek(0)
    #     if (len(lines) >= index + 10):  # if there is new data
    #         index += 10
    #         inp = lines[index - 10:index]  # get data for next frame
    #         # Which Y-position is lower?
    #         if (float(inp[7]) < float(inp[8])):  # Then use inp[5] because it's the smallest Y-point
    #             if (lowest_y_point > float(inp[7])):
    #                 lowest_y_point = float(inp[7])
    #         else:
    #             if (lowest_y_point > float(inp[8])):
    #                 lowest_y_point = float(inp[8])

    # # print("LOWEST_Y_POINT === " + str(lowest_y_point))
    # # End of initialization step

    # file = open('real_time_joints_data.txt', 'w+')
    # index = 0

    # # Start system
    # while True:
        # os.system('cls')
        # global posture
        # lines = file.read().splitlines()
        # file.seek(0)  # move cursor to beggining of file for next loop
        # if (len(lines) >= index + 10):  # if there is new data
        #     index += 10
        #     inp = lines[index - 10:index]  # get data for next frame
        #     # index += 20 #10 FPS
        #     inp = [float(i) for i in inp]
        #     inputVals = np.random.rand(1, 7)
        #     inputVals[0] = inp[:7]  # Only the first 7 values. The other two values will be used to check the floor plan
        #     posture = getLSTMClassification(inputVals)

        #     labelText.set(getTime() + " " + posture)
        #     root.update()

        #     # print(getTime() + " " + posture)
        #     if (posture == "LYING DOWN"):
        #         if (isLayingOnTheFloor(float(inp[7]), float(inp[8]))):
        #             timestamp = inp[9]
        #             fall = True
        #             allowed = 2  # at least 95% of the time detected as LYING DOWN.
        #             allowed_not_on_floor = 5
        #             for i in range(20):  # check LYING DOWN for 2 seconds (10fps*2s = 20 frames)
        #                 while (len(lines) < index + 10):
        #                     lines = file.read().splitlines()
        #                     file.seek(0)  # move cursor to beggining of file for next loop
        #                 index += 10
        #                 inp = lines[index - 10:index]  # get data for next frame
        #                 # index += 20 #10 FPS
        #                 inp = [float(i) for i in inp]
        #                 inputVals = np.random.rand(1, 7)
        #                 inputVals[0] = inp[:7]
        #                 # timestamps.append(inp[9])
        #                 posture = getLSTMClassification(inputVals)
        #                 # print(datetime.datetime.now(), posture)

        #                 labelText.set(getTime() + " " + posture)
        #                 root.update()

        #                 if (posture == "LYING DOWN"):  # Is the person LYING DOWN on the floor?
        #                     if (isLayingOnTheFloor(float(inp[7]), float(inp[8])) == False):
        #                         if (allowed_not_on_floor == 0):
        #                             print("PERSON IS NOT LAYING ON THE FLOOR! Not fall..!")
        #                             fall = False
        #                             break
        #                         else:
        #                             allowed_not_on_floor -= 1
        #                 else:  # 10% allowed to not be LYING DOWN (2/20)
        #                     if (allowed == 0):
        #                         print("PERSON HAS NOT BEEN LAYING ON THE FLOOR FOR MORE THAN 2 SECONDS! Not fall..!")
        #                         fall = False
        #                         break
        #                     else:
        #                         allowed -= 1
        #             if (fall):
        #                 labelText.set(getTime() + " " + "FALLEN!")
        #                 root.update()
        #                 # print(getTime() + " " + "--FALLEN!--")

        #                 # You can now reset index=0 and delete the file to restart the While loop from current data.
        #                 while posture == "LYING DOWN":  # Fallen until detected in another posture
        #                     while (len(lines) < index + 10):
        #                         lines = file.read().splitlines()
        #                         file.seek(0)  # move cursor to beggining of file for next loop
        #                     index += 10
        #                     inp = lines[index - 9:index]  # get data for next frame
        #                     inp = [float(i) for i in inp]
        #                     inputVals = np.random.rand(1, 7)
        #                     inputVals[0] = inp[:7]
        #                     posture = getLSTMClassification(inputVals)
        #                     # print(getTime() + " " + posture)
        #                     if posture != "LYING DOWN":
        #                         labelText.set(getTime() + " " + posture)
        #                         root.update()
        #                         file = open('real_time_joints_data.txt', 'w+')
        #                         index = 0
        # if (index > 2500):
        #     # index = 300
        #     file = open('real_time_joints_data.txt', 'w+')
        #     index = 0