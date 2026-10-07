#include "backgroundpool.hpp"

#include <qthreadpool.h>

QThreadPool* BackgroundThreadPool::instance() {
	static auto* pool = []() {
		auto* pool = new QThreadPool(); // NOLINT
		pool->setMaxThreadCount(1);
		return pool;
	}();

	return pool;
}
