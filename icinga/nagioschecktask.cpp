#include "i2-icinga.h"

#ifndef _MSC_VER
#	include "popen_noshell.h"
#endif /* _MSC_VER */

using namespace icinga;

boost::mutex NagiosCheckTask::m_Mutex;
vector<NagiosCheckTask::Ptr> NagiosCheckTask::m_PendingTasks;
deque<NagiosCheckTask::Ptr> NagiosCheckTask::m_Tasks;
condition_variable NagiosCheckTask::m_TasksCV;

NagiosCheckTask::NagiosCheckTask(const Service& service)
	: CheckTask(service), m_FP(NULL), m_UsePopen(false)
{
	string checkCommand = service.GetCheckCommand();
	m_Command = MacroProcessor::ResolveMacros(checkCommand, service.GetMacros()); // + " 2>&1";
}

void NagiosCheckTask::Enqueue(void)
{
	time_t now;
	time(&now);
	GetResult().SetScheduleStart(now);

	m_PendingTasks.push_back(GetSelf());
}

void NagiosCheckTask::FlushQueue(void)
{
	{
		mutex::scoped_lock lock(m_Mutex);
		std::copy(m_PendingTasks.begin(), m_PendingTasks.end(), back_inserter(m_Tasks));
		m_PendingTasks.clear();
		m_TasksCV.notify_all();
	}
}

void NagiosCheckTask::CheckThreadProc(void)
{
	mutex::scoped_lock lock(m_Mutex);

	map<int, NagiosCheckTask::Ptr> tasks;

	for (;;) {
		while (m_Tasks.empty() || tasks.size() >= MaxChecksPerThread) {
			lock.unlock();

			map<int, NagiosCheckTask::Ptr>::iterator it, prev;

#ifndef _MSC_VER
			fd_set readfds;
			int nfds = 0;
			
			FD_ZERO(&readfds);

			for (it = tasks.begin(); it != tasks.end(); it++) {
				if (it->first > nfds)
					nfds = it->first;

				FD_SET(it->first, &readfds);
			}

			timeval tv;
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			select(nfds + 1, &readfds, NULL, NULL, &tv);
#else /* _MSC_VER */
			Sleep(1000);
#endif /* _MSC_VER */

			for (it = tasks.begin(); it != tasks.end(); ) {
				int fd = it->first;
				NagiosCheckTask::Ptr task = it->second;

#ifndef _MSC_VER
				if (!FD_ISSET(fd, &readfds)) {
					it++;
					continue;
				}
#endif /* _MSC_VER */

				if (!task->RunTask()) {
					time_t now;
					time(&now);
					task->GetResult().SetScheduleEnd(now);

					CheckTask::FinishTask(task);
					prev = it;
					it++;
					tasks.erase(prev);
				} else {
					it++;
				}
			}

			lock.lock();
		}

		while (!m_Tasks.empty() && tasks.size() < MaxChecksPerThread) {
			NagiosCheckTask::Ptr task = m_Tasks.front();
			m_Tasks.pop_front();
			if (!task->InitTask()) {
				time_t now;
				time(&now);
				task->GetResult().SetScheduleEnd(now);

				CheckTask::FinishTask(task);
			} else {
				int fd = task->GetFD();
				if (fd >= 0)
					tasks[fd] = task;
			}
		}
	}
}

bool NagiosCheckTask::InitTask(void)
{
#ifdef _MSC_VER
	m_FP = _popen(m_Command.c_str(), "r");
#else /* _MSC_VER */
	if (!m_UsePopen) {
		m_PCloseArg = new popen_noshell_pass_to_pclose;

		m_FP = popen_noshell_compat(m_Command.c_str(), "r", (popen_noshell_pass_to_pclose *)m_PCloseArg);

		if (m_FP == NULL) // TODO: add check for valgrind
			m_UsePopen = true;
	}

	if (m_UsePopen)
		m_FP = popen(m_Command.c_str(), "r");
#endif /* _MSC_VER */

	if (m_FP == NULL) {
		time_t now;
		time(&now);
		GetResult().SetExecutionEnd(now);

		return false;
	}

	return true;
}

bool NagiosCheckTask::RunTask(void)
{
	char buffer[512];
	size_t read = fread(buffer, 1, sizeof(buffer), m_FP);

	if (read > 0)
		m_OutputStream.write(buffer, read);

	if (!feof(m_FP))
		return true;

	string output = m_OutputStream.str();
	boost::algorithm::trim(output);
	GetResult().SetOutput(output);

	int status, exitcode;
#ifdef _MSC_VER
	status = _pclose(m_FP);
#else /* _MSC_VER */
	if (m_UsePopen) {
		status = pclose(m_FP);
	} else {
		status = pclose_noshell((popen_noshell_pass_to_pclose *)m_PCloseArg);
		delete (popen_noshell_pass_to_pclose *)m_PCloseArg;
	}
#endif /* _MSC_VER */

#ifndef _MSC_VER
	if (WIFEXITED(status)) {
		exitcode = WEXITSTATUS(status);
#else /* _MSC_VER */
		exitcode = status;
#endif /* _MSC_VER */

		ServiceState state;

		switch (exitcode) {
			case 0:
				state = StateOK;
				break;
			case 1:
				state = StateWarning;
				break;
			case 2:
				state = StateCritical;
				break;
			default:
				state = StateUnknown;
				break;
		}

		GetResult().SetState(state);
#ifndef _MSC_VER
	} else if (WIFSIGNALED(status)) {
		stringstream outputbuf;
		outputbuf << "Process was terminated by signal " << WTERMSIG(status);
		GetResult().SetOutput(outputbuf.str());
		GetResult().SetState(StateUnknown);
	}
#endif /* _MSC_VER */

	time_t now;
	time(&now);
	GetResult().SetExecutionEnd(now);

	return false;
}

int NagiosCheckTask::GetFD(void) const
{
	return fileno(m_FP);
}

CheckTask::Ptr NagiosCheckTask::CreateTask(const Service& service)
{
	return boost::make_shared<NagiosCheckTask>(service);
}

void NagiosCheckTask::Register(void)
{
	CheckTask::RegisterType("nagios", NagiosCheckTask::CreateTask, NagiosCheckTask::FlushQueue);

	int numThreads = boost::thread::hardware_concurrency();

	if (numThreads < 4)
		numThreads = 4;

	for (int i = 0; i < numThreads; i++) {
		thread t(&NagiosCheckTask::CheckThreadProc);
		t.detach();
	}
}
