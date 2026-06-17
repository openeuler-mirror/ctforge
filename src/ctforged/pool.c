#include "ctforge.h"

struct ctforge_threadpool *workpool;

struct ctforge_threadpool *threadpool_create(int thread_count, int queue_size)
{
	struct ctforge_threadpool *pool;
	int i;

	if (thread_count <= 0 || thread_count > MAX_THREADS ||
	    queue_size <= 0 || queue_size > QUEUE_SIZE) {
		return NULL;
	}

	pool = (struct ctforge_threadpool *)malloc(
		sizeof(struct ctforge_threadpool));
	if (pool == NULL)
		return NULL;

	pool->thread_count = 0;
	pool->queue_size = queue_size;
	pool->head = pool->tail = pool->count = 0;
	pool->shutdown = pool->started = 0;

	pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
	pool->tasks = (struct ctforge_task *)malloc(
		sizeof(struct ctforge_task) * queue_size);

	if ((pthread_mutex_init(&(pool->lock), NULL) != 0) ||
	    (pthread_cond_init(&(pool->notify), NULL) != 0) ||
	    (pool->threads == NULL) || (pool->tasks == NULL)) {
		return NULL;
	}

	for (i = 0; i < thread_count; i++) {
		if (pthread_create(&(pool->threads[i]), NULL, threadpool_thread,
				   (void *)pool) != 0) {
			threadpool_destroy(pool);
			return NULL;
		}
		pool->thread_count++;
		pool->started++;
	}

	return pool;
}

int threadpool_add(struct ctforge_threadpool *pool, void (*function)(void *),
		   void *arg)
{
	int next, err = 0;

	if (pool == NULL || function == NULL)
		return -1;

	if (pthread_mutex_lock(&(pool->lock)) != 0)
		return -1;

	next = (pool->tail + 1) % pool->queue_size;

	do {
		if (pool->count == pool->queue_size) {
			err = -1;
			break;
		}

		if (pool->shutdown) {
			err = -1;
			break;
		}

		pool->tasks[pool->tail].function = function;
		pool->tasks[pool->tail].arg = arg;
		pool->tail = next;
		pool->count += 1;

		if (pthread_cond_signal(&(pool->notify)) != 0) {
			err = -1;
			break;
		}
	} while (0);

	if (pthread_mutex_unlock(&pool->lock) != 0)
		err = -1;

	return err;
}

void *threadpool_thread(void *threadpool)
{
	struct ctforge_threadpool *pool =
		(struct ctforge_threadpool *)threadpool;
	struct ctforge_task task;

	while (1) {
		pthread_mutex_lock(&(pool->lock));

		while ((pool->count == 0) && (!pool->shutdown))
			pthread_cond_wait(&(pool->notify), &(pool->lock));

		if ((pool->shutdown == 1) ||
		    ((pool->shutdown == 2) && (pool->count == 0))) {
			break;
		}

		task.function = pool->tasks[pool->head].function;
		task.arg = pool->tasks[pool->head].arg;
		pool->head = (pool->head + 1) % pool->queue_size;
		pool->count -= 1;

		pthread_mutex_unlock(&(pool->lock));

		(*(task.function))(task.arg);
	}

	pool->started--;

	pthread_mutex_unlock(&(pool->lock));
	pthread_exit(NULL);
	return NULL;
}

int threadpool_destroy(struct ctforge_threadpool *pool)
{
	int i, err = 0;

	if (pool == NULL)
		return -1;

	if (pthread_mutex_lock(&(pool->lock)) != 0)
		return -1;

	do {
		if (pool->shutdown) {
			err = -1;
			break;
		}

		pool->shutdown = 1;

		if ((pthread_cond_broadcast(&(pool->notify)) != 0) ||
		    (pthread_mutex_unlock(&(pool->lock)) != 0)) {
			err = -1;
			break;
		}

		for (i = 0; i < pool->thread_count; i++) {
			if (pthread_join(pool->threads[i], NULL) != 0)
				err = -1;
		}
	} while (0);

	if (!err)
		threadpool_free(pool);

	return err;
}

int threadpool_free(struct ctforge_threadpool *pool)
{
	if (pool == NULL || pool->started > 0)
		return -1;

	if (pool->threads) {
		free(pool->threads);
		free(pool->tasks);

		pthread_mutex_lock(&(pool->lock));
		pthread_mutex_destroy(&(pool->lock));
		pthread_cond_destroy(&(pool->notify));
	}
	free(pool);
	return 0;
}

int threadpoll_init(void)
{
	workpool = threadpool_create(MAX_THREADS, QUEUE_SIZE);
	pr_info("workpool inited");
	return 0;
}
