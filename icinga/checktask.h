#ifndef CHECKTASK_H
#define CHECKTASK_H

namespace icinga
{

struct CheckTaskType;

class I2_ICINGA_API CheckTask : public Object
{
public:
	typedef shared_ptr<CheckTask> Ptr;
	typedef weak_ptr<CheckTask> WeakPtr;

	typedef function<CheckTask::Ptr(const Service&)> Factory;
	typedef function<void()> QueueFlusher;

	Service& GetService(void);
	CheckResult& GetResult(void);

	virtual void Enqueue(void) = 0;

	static void RegisterType(string type, Factory factory, QueueFlusher qflusher);
	static CheckTask::Ptr CreateTask(const Service& service);
	static void Enqueue(const CheckTask::Ptr& task);
	static void FlushQueue(void);

	static void FinishTask(const CheckTask::Ptr& task);
	static vector<CheckTask::Ptr> GetFinishedTasks(void);

protected:
	CheckTask(const Service& service);

private:
	Service m_Service;
	CheckResult m_Result;

	static map<string, CheckTaskType> m_Types;

	static vector<CheckTask::Ptr> m_FinishedTasks;
	static mutex m_FinishedTasksMutex;
};

struct CheckTaskType
{
	CheckTask::Factory Factory;
	CheckTask::QueueFlusher QueueFlusher;
};

}

#endif /* CHECKTASK_H */
