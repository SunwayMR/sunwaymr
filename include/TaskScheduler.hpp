/*
 * TaskScheduler.hpp
 *
 */

#ifndef TASKSCHEDULER_HPP_
#define TASKSCHEDULER_HPP_

#include "TaskScheduler.h"

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <stdlib.h>

#include "Messaging.hpp"
#include "MessageType.hpp"
#include "Logging.hpp"
#include "Utils.hpp"
#include "Task.hpp"
#include "TaskResult.hpp"
#include "StringConversion.hpp"

using namespace std;

/*
 * constructor
 */
template<class T>
TaskScheduler<T>::TaskScheduler(int jobID, string selfIP, int selfIPIndex,
		string master, string appName, int listenPort,
		vector<string> &ip,
		vector<int> &threads,
		vector<int> &memory)
		: jobID(jobID), selfIP(selfIP), selfIPIndex(selfIPIndex), 
				master(master), appName(appName), listenPort(listenPort),
				IPVector(ip),
				threadCountVector(threads),
				memoryVector(memory),
				isMaster(0) {
	if (master == "local" || master == selfIP) {
		isMaster = 1;
	}
	runningThreadNum = 0;
	allTaskResultsReceived = false;
	taskResultListSent = false;
	receivedTaskResultNum = 0;
	pthread_mutex_init(&mutex_handle_message_ready, NULL); // initialize mutex
	pthread_mutex_lock(&mutex_handle_message_ready);
	pthread_mutex_init(&mutex_task_scheduler, NULL); // initialize mutex
}

/*
 * destructor
 */
template<class T>
TaskScheduler<T>::~TaskScheduler() {

}

/*
 * thread data for task threads
 */
template<class T>
struct xyz_task_scheduler_thread_data_ {
	TaskScheduler<T> *taskScheduler;
	string master;
	int masterPort;
	int jobID;
	int taskID;
	Task<T> *task;

	xyz_task_scheduler_thread_data_(TaskScheduler<T> *t, string m, int mp, int ji, int ti,
			Task<T> *task) :
			taskScheduler(t), master(m),
			masterPort(mp), jobID(ji),
			taskID(ti), task(task) {
	}
};

/*
 * thread function for running tasks.
 * every task will run in separate threads.
 */
template<class T>
void *xyz_task_scheduler_run_task_(void *d) {
	struct xyz_task_scheduler_thread_data_<T> *data = (struct xyz_task_scheduler_thread_data_<T> *) d;
	TaskScheduler<T> *ts = data->taskScheduler;
	Task<T> *task = data->task;
	T value = task->run();
	ts->finishTask(data->taskID, value);

	pthread_exit(NULL);
}

/*
 * before running tasks, do preparation work
 */
template<class T>
void TaskScheduler<T>::preRunTasks(vector< Task<T> * > &tasks) {
	stringstream runTasksInfo;
	runTasksInfo << "TaskScheduler: preRunTasks, job: [" << jobID
			<< "], tasks: [" << tasks.size() << "]";
	Logging::logInfo(runTasksInfo.str());

	int taskNum = tasks.size();
	this->tasks = tasks;
	runningThreadNum = 0;
	allTaskResultsReceived = false;
	receivedTaskResultNum = 0;
	resultReceived = vector<bool>(taskNum, false);
	taskResults = vector< TaskResult<T>* >(taskNum, NULL);

	pthread_mutex_init(&mutex_all_tasks_received, NULL);
	pthread_mutex_lock(&mutex_all_tasks_received);
	pthread_mutex_unlock(&mutex_handle_message_ready);
}

/*
 * to run tasks
 */
template<class T>
vector<TaskResult<T>*> TaskScheduler<T>::runTasks(vector<Task<T>*> &tasks) {
	const int taskNum = tasks.size();
	const int totalThreads = vectorSum(threadCountVector);

	taskOnIPVector = vector<string>(taskNum, selfIP);
	vector<int> needReDistributing(taskNum, 1);
	int currentTask = 0;
	while(currentTask < taskNum) {
		// to distribute tasks, to decide which node a task runs on
		// distributiont.step.1
		vector<int> threadRemainVector = threadCountVector;
		// distribute [totalThreads] tasks per round.
		// this is optimized for [join] which increases tasks count.
		int t = taskNum - currentTask;
		if(t > totalThreads) t = totalThreads;

		// distributiont.step.2
		// preferredLocations
		for (int i = currentTask; i < currentTask + t; i++) {
			if (tasks[i]->preferredLocations().size() > 0) {
				int index = -1;

				int j;
				int plSize = tasks[i]->preferredLocations().size();
				for (j = 0; j < plSize; j++) {
					index = vectorFind(IPVector, tasks[i]->preferredLocations()[j]);

					if (index != -1 && threadRemainVector[index] > 0) {
						threadRemainVector[index]--;
						taskOnIPVector[i] = IPVector[index];
						needReDistributing[i] = 0;

						stringstream taskOnIP;
						taskOnIP << "TaskScheduler: task [" << i << "] of job["
								<< jobID << "] will run at " << taskOnIPVector[i];
						Logging::logDebug(taskOnIP.str());

						break;
					}
				}
			}
		}

		// distributiont.step.3
		// re-distribution. reason:
		//    1.preferred locations do not meet with resources demand
		//    2.no preferred locations

		for (int i = currentTask; i < currentTask + t; i++) {
			if (needReDistributing[i] == 1) {
				int index = vectorNonZero(threadRemainVector);
				if (index != -1) {
					threadRemainVector[index]--;
					taskOnIPVector[i] = IPVector[index];
					needReDistributing[i] = 0;

					stringstream taskOnIP;
					taskOnIP << "TaskScheduler: task [" << i << "] of job[" << jobID
							<< "] will run at " << taskOnIPVector[i];
					Logging::logDebug(taskOnIP.str());
				} else {
					//no thread left when heavy load
					//threadRemainVector is big enough to deal all tasks
				}
			}
		}

		currentTask += t;
	}

	// task distribution finished

	// run tasks those been distributed to this node
	int runOnThisNodeTaskNum = 0;
	int lanuchedTaskNum = 0;
	for (int i = 0; i < taskNum; i++) {
		if (taskOnIPVector[i] == selfIP)
			runOnThisNodeTaskNum++;
	}
	vector<int> launchedTask = vector<int>(taskNum);

	while (!allTaskResultsReceived) { // waiting until all results received
		if (lanuchedTaskNum == runOnThisNodeTaskNum) {
			pthread_mutex_lock(&mutex_all_tasks_received);
			pthread_mutex_unlock(&mutex_all_tasks_received);
			pthread_mutex_destroy(&mutex_all_tasks_received);
			continue;
		}

		if (XYZ_TASK_SCHEDULER_RUN_TASK_MODE == 0) {
			// [0]run tasks by fork
			while (lanuchedTaskNum < runOnThisNodeTaskNum) {
				for (int i = 0; i < taskNum; i++) {
					if (taskOnIPVector[i] == selfIP && launchedTask[i] == 0) { // pick non started task
						if (fork() == 0) { // !!! keep thread safety !!! no stdio !!!
							T value = tasks[i]->run();
							this->finishTask(i, value);

							exit(0);
						} else {
							launchedTask[i] = 1;
							lanuchedTaskNum++;
						}
					}
				}
			}

		} else {
			// [1]run tasks by pthread
			int THREADS_NUM_MAX = threadCountVector[selfIPIndex]; // TODO configuration out of code
			while (runningThreadNum < THREADS_NUM_MAX
					&& lanuchedTaskNum < runOnThisNodeTaskNum) {
				for (int i = 0; i < taskNum; i++) {
					if (taskOnIPVector[i] == selfIP && launchedTask[i] == 0) { // pick non started task
						pthread_t thread;
						struct xyz_task_scheduler_thread_data_<T> *data =
								new xyz_task_scheduler_thread_data_<T>(this,
										master, listenPort, jobID, i, tasks[i]);
						int rc = pthread_create(&thread, NULL, xyz_task_scheduler_run_task_<T>,
								(void *) data);
						if (rc) {
							Logging::logError(
									"TaskScheduler: failed to create thread to run task");
							exit(-1);
						}
						startedThreads.push_back(thread);

						launchedTask[i] = 1;
						lanuchedTaskNum++;
						increaseRunningThreadNum();
						break;
					}
				}
			}
		} // end of if...else   running mode

	} // end of while

	if(Logging::getMask() <= 0) {
		stringstream results;
		results << "TaskScheduler: \n" << "job[" << jobID << "] task results: \n";
		for (unsigned int i = 0; i < taskResults.size(); i++) {
			results << "[" << i << "] "
					<< taskResults[i]->task->serialize(taskResults[i]->value)
					<< endl;
		}
		Logging::logVerbose(results.str());
	}

	return taskResults;
}

/*
 * to finish a task with task index and result
 */
template <class T>
void TaskScheduler<T>::finishTask(int task, T &value) {
	if (task>=0 && (unsigned)task<this->tasks.size()) {
		// to send out task result
		string msg = "";
		msg = msg + to_string(this->jobID)
				+ TASK_RESULT_DELIMITATION
				+ to_string(task)
				+ TASK_RESULT_DELIMITATION
				+ this->tasks[task]->serialize(value);

		usleep(rand()%500000); // delay sending result
		this->sendMessage(this->master, this->listenPort, A_TASK_RESULT, msg);
		if(XYZ_TASK_SCHEDULER_RUN_TASK_MODE == 1) {
			this->decreaseRunningThreadNum();
		}

	}
}

/*
 * override messageReceived from Messaging
 */
template<class T>
void TaskScheduler<T>::messageReceived(
		int localListenPort, string fromHost,
		int msgType, string &msg) {

}

/*
 * override handleMessage from Scheduler.
 * to handle messages from JobScheduler.
 */
template<class T>
void TaskScheduler<T>::handleMessage(
		int localListenPort, string fromHost,
		int msgType, string &msg, int &retValue) {
	pthread_mutex_lock(&mutex_handle_message_ready);
	pthread_mutex_unlock(&mutex_handle_message_ready);

	switch (msgType) {
	case A_TASK_RESULT: {
		if (isMaster == 1) { // && (unsigned)receivedTaskResultNum < tasks.size()) {

			// add task result to taskResults, if not duplicate
			vector<string> vs;
			splitString(msg, vs, TASK_RESULT_DELIMITATION);
			if (vs.size() >= 2) {
				int jobID = atoi(vs[0].c_str());
				int taskID = atoi(vs[1].c_str());

				if (jobID == this->jobID && (unsigned)taskID < tasks.size()
						&& !resultReceived[taskID]) {
					if (vs.size() >= 3) {
						T value = tasks[taskID]->deserialize(vs[2]);
						taskResults[taskID] =
								new TaskResult<T>(tasks[taskID], value);

					} else {
						string rs = "";
						T value = tasks[taskID]->deserialize(rs);
						taskResults[taskID] =
								new TaskResult<T>(tasks[taskID], value);
					}

					// lock mutex
					pthread_mutex_lock(&mutex_task_scheduler);
					resultReceived[taskID] = true;
					receivedTaskResultNum++;
					stringstream receivedDebug;
					receivedDebug
							<< "TaskScheduler: master: a task result of job["
							<< jobID << "] received, totally ["
							<< receivedTaskResultNum << "/"
							<< tasks.size() << "] received";
					Logging::logDebug(receivedDebug.str());

					// unlock mutex
					pthread_mutex_unlock(&mutex_task_scheduler);
				} else if (jobID > this->jobID) { // task result for next job
					retValue = jobID;
					break;
				}
			}

			// check if all task results received
			// lock mutex
			pthread_mutex_lock(&mutex_task_scheduler);
			if (!taskResultListSent && (unsigned)receivedTaskResultNum == tasks.size()) {
				allTaskResultsReceived = true;
				Logging::logInfo(
						"TaskScheduler: master: sending out results...");

				// send out to other nodes
				string msg;
				this->getTaskResultListString(this->jobID, msg);

				for (unsigned int i = 0; i < IPVector.size(); i++) {
					if(IPVector[i] == selfIP) continue;
					sendMessage(IPVector[i], listenPort, TASK_RESULT_LIST, msg);
				}
				taskResultListSent = true;

				Logging::logInfo("TaskScheduler: results sent");
				// unlock mutex for waiting all task results received
				pthread_mutex_unlock(&mutex_all_tasks_received);
			}
			// unlock mutex
			pthread_mutex_unlock(&mutex_task_scheduler);
		}

		break;
	}

	case TASK_RESULT_LIST: { // task result list from master
		if (isMaster == 0) {

			Logging::logInfo("TaskScheduler: task result list received");

			// save task results
			// initialize taskResults
			bool valid = true;
			vector<string> results;
			splitString(msg, results, TASK_RESULT_LIST_DELIMITATION);
			if (results.size() == tasks.size()) {
				for (unsigned int i = 0; i < tasks.size(); i++) {
					vector<string> vs;
					splitString(results[i], vs, TASK_RESULT_DELIMITATION);
					if (vs.size() >= 2) {
						int jobID = atoi(vs[0].c_str());
						int taskID = atoi(vs[1].c_str());

						if (jobID == this->jobID && (unsigned)taskID < tasks.size()
								&& !resultReceived[taskID]) {
							if (vs.size() >= 3) {
								T value = tasks[taskID]->deserialize(vs[2]);
								taskResults[i] =
										new TaskResult<T>(tasks[taskID],
												value);
							} else {
								string rs = "";
								T value = tasks[taskID]->deserialize(rs);
								taskResults[i] =
										new TaskResult<T>(tasks[taskID],
												value);
							}
							resultReceived[taskID] = true;
							receivedTaskResultNum++;
						} else {
							valid = false;
							break;
						}
					} else {
						valid = false;
						break;
					}
				}
			} else {
				valid = false;
			}

			if (valid) {
				allTaskResultsReceived = true;
				pthread_mutex_unlock(&mutex_all_tasks_received);

			} else { // error
				Logging::logError(
						"TaskScheduler: task result list invalid");
			}
		} else {
			// master had all results
		}

		break;
	}
	case RESULT_RENEED: {
		if (isMaster == 0) {

		}
		break;
	}

	case RESULT_RENEED_TOTAL: {
		if (isMaster == 1) {

		}
		break;
	}

	}
}

/*
 * to increase running thread number
 */
template<class T>
void TaskScheduler<T>::increaseRunningThreadNum() {
	pthread_mutex_lock(&mutex_task_scheduler);
	runningThreadNum++;
	pthread_mutex_unlock(&mutex_task_scheduler);
}

/*
 * to decrease running thread number
 */
template<class T>
void TaskScheduler<T>::decreaseRunningThreadNum() {
	pthread_mutex_lock(&mutex_task_scheduler);
	runningThreadNum--;
	pthread_mutex_unlock(&mutex_task_scheduler);
}

/*
 * to get a task result as string
 */
template<class T>
bool TaskScheduler<T>::getTaskResultString(int job, int task, string &result) {
	if (job != this->jobID) return false;
	if ((unsigned)task >= this->resultReceived.size()
			|| !this->resultReceived[task]) return false;
	if (this->taskResults[task] != NULL) {
		T& value = taskResults[task]->value;
		result = "";
		result = result + to_string(jobID)
				+ TASK_RESULT_DELIMITATION
				+ to_string(task)
				+ TASK_RESULT_DELIMITATION
				+ tasks[task]->serialize(value);
		return true;
	}
	return false;
}

/*
 * to get task result list as string
 */
template<class T>
bool TaskScheduler<T>::getTaskResultListString(int job, string &result) {
	if (job != this->jobID) return false;
	if (!this->allTaskResultsReceived) return false;

	result = "";
	for (unsigned int i=0; i<this->tasks.size(); i++) {
		string tr;
		this->getTaskResultString(job, i, tr);
		result += tr;
		if (i != this->tasks.size()-1) {
			result += TASK_RESULT_LIST_DELIMITATION;
		}
	}
	return true;
}

#endif /* TASKSCHEDULER_HPP_ */

