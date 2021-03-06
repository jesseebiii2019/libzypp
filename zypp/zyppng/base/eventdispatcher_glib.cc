#include "eventdispatcher.h"
#include "timer.h"
#include "private/eventdispatcher_glib_p.h"

#include <zypp/base/Exception.h>
#include <zypp/base/Logger.h>
namespace zyppng {

static int inline readMask () {
  return ( G_IO_IN | G_IO_HUP );
}

static int inline writeMask () {
  return ( G_IO_OUT );
}

static int inline excpMask () {
  return ( G_IO_PRI );
}

//returns the thread local dispatcher, we only support one EventDispatcher per thread
static EventDispatcher **threadLocalDispatcher ( EventDispatcher * set = nullptr )
{
  static __thread EventDispatcher *threadDispatch = nullptr;
  if ( set ) {
    if ( threadDispatch )
      ZYPP_THROW( zypp::Exception( "EventDispatcher can only be created once per thread" ) );
    threadDispatch = set;
  }
  return &threadDispatch;
}

static GSourceFuncs abstractEventSourceFuncs = {
  GAbstractEventSource::prepare,
  GAbstractEventSource::check,
  GAbstractEventSource::dispatch,
  nullptr,
  nullptr,
  nullptr
};

GAbstractEventSource *GAbstractEventSource::create(EventDispatcherPrivate *ev ) {
  GAbstractEventSource *src = nullptr;
  src = reinterpret_cast<GAbstractEventSource *>(g_source_new(&abstractEventSourceFuncs, sizeof(GAbstractEventSource)));
  (void) new (&src->pollfds) std::vector<GUnixPollFD>();

  src->eventSource = nullptr;
  src->_ev = ev;
  return src;
}

void GAbstractEventSource::destruct(GAbstractEventSource *src)
{
  for ( GUnixPollFD &fd : src->pollfds ) {
    if ( fd.tag )
      g_source_remove_unix_fd( &src->source, fd.tag );
  }

  src->pollfds.clear();
  src->pollfds.std::vector< GUnixPollFD >::~vector();
  g_source_destroy( &src->source );
  g_source_unref( &src->source );
}

gboolean GAbstractEventSource::prepare(GSource *, gint *timeout)
{
  //we can not yet determine if the GSource is ready, polling FDs also have no
  //timeout, so lets continue
  if ( timeout )
    *timeout = -1;
  return false;
}

//here we need to figure out which FDs are pending
gboolean GAbstractEventSource::check( GSource *source )
{
  GAbstractEventSource *src = reinterpret_cast<GAbstractEventSource*>( source );

  //check for pending and remove orphaned entries
  bool hasPending = false;

  for ( auto fdIt = src->pollfds.begin(); fdIt != src->pollfds.end(); ) {
    if ( fdIt->tag == nullptr ) {
      //this pollfd was removed, clear it from the list
      //for now keep the object in the sources list if the pollfd list gets empty, if it does not register new events until
      //next check it is removed for good
      fdIt = src->pollfds.erase( fdIt );
    } else {
      GIOCondition pendEvents = g_source_query_unix_fd( source, fdIt->tag );
      if ( pendEvents & G_IO_NVAL ){
        //that poll is broken, do we need to do more????
        fdIt = src->pollfds.erase( fdIt );
      } else {
        hasPending = hasPending || ( pendEvents & fdIt->reqEvents );
        fdIt++;
      }
    }
  }

  //if the pollfds are empty trigger dispatch so this source can be removed
  return hasPending || src->pollfds.empty();
}

//Trigger all event sources that have been activated
gboolean GAbstractEventSource::dispatch(GSource *source, GSourceFunc, gpointer)
{
  GAbstractEventSource *src = reinterpret_cast<GAbstractEventSource*>( source );

  if ( !src )
    return G_SOURCE_REMOVE;

  //sources are only removed here so we do not accidentially mess with the pollfd iterator in the next loop
  //were we trigger all ready FDs
  if ( src->pollfds.empty() ) {
    auto it = std::find( src->_ev->_eventSources.begin(), src->_ev->_eventSources.end(), src );

    if ( it != src->_ev->_eventSources.end() ) {
      GAbstractEventSource::destruct( *it );
      src->_ev->_eventSources.erase( it );
      return G_SOURCE_REMOVE;
    }
  }

  for ( const GUnixPollFD &pollfd : src->pollfds ) {
    //do not trigger orphaned ones
    if ( pollfd.tag != nullptr ) {
      GIOCondition pendEvents = g_source_query_unix_fd( source, pollfd.tag );

      if ( (pendEvents & pollfd.reqEvents ) != 0 ) {
        int ev = 0;
        if ( ( pendEvents & readMask() ) && ( pollfd.reqEvents & readMask() ) )
          ev = AbstractEventSource::Read;
        if ( (pendEvents & writeMask() ) && ( pollfd.reqEvents & writeMask() ) )
          ev = ev | AbstractEventSource::Write;
        if ( (pendEvents & excpMask()) && ( pollfd.reqEvents & excpMask() ) )
          ev = ev | AbstractEventSource::Exception;
        if ( (pendEvents & G_IO_ERR) && ( pollfd.reqEvents & G_IO_ERR ) )
          ev = ev | AbstractEventSource::Error;

        src->eventSource->onFdReady( pollfd.pollfd, ev );
      }
    }
  }

  return G_SOURCE_CONTINUE;
}

static GSourceFuncs glibTimerSourceFuncs = {
  GLibTimerSource::prepare,
  GLibTimerSource::check,
  GLibTimerSource::dispatch,
  nullptr,
  nullptr,
  nullptr
};

//check when this timer expires and set the correct timeout
gboolean GLibTimerSource::prepare(GSource *src, gint *timeout)
{
  GLibTimerSource *source = reinterpret_cast<GLibTimerSource *>( src );
  if ( !source )
    return false; //not ready for dispatch

  if ( !source->_t )
    return false;

  uint64_t nextTimeout = source->_t->remaining();
  if ( timeout ) {
    //this would be a really looong timeout, but be safe
    if ( nextTimeout > G_MAXINT )
      *timeout = G_MAXINT;
    else
      *timeout = static_cast<gint>( nextTimeout );
  }
  return ( nextTimeout == 0 );
}

//this is essentially the same as prepare
gboolean GLibTimerSource::check(GSource *source)
{
  return prepare( source, nullptr );
}

//emit the expired timers, restart timers that are no single shots
gboolean GLibTimerSource::dispatch(GSource *src, GSourceFunc, gpointer)
{
  GLibTimerSource *source = reinterpret_cast<GLibTimerSource *>( src );
  if ( !source )
    return true;

  if ( source->_t == nullptr )
    return true;
  //this will emit the expired signal and reset the timer
  //or stop it in case its a single shot timer
  source->_t->expire();
  return true;
}

GLibTimerSource *GLibTimerSource::create()
{
  GLibTimerSource *src = nullptr;
  src = reinterpret_cast<GLibTimerSource *>(g_source_new(&glibTimerSourceFuncs, sizeof(GLibTimerSource)));
  src->_t = nullptr;
  return src;
}

void GLibTimerSource::destruct(GLibTimerSource *src)
{
  g_source_destroy( &src->source );
  g_source_unref( &src->source );
}

/*!
 * \brief Called when the event loop is idle, here we run cleanup tasks and call later() callbacks of the user
 */
static gboolean  eventLoopIdleFunc ( gpointer user_data )
{
  auto dPtr = reinterpret_cast<EventDispatcherPrivate *>( user_data );
  if ( dPtr ) {
    if( dPtr->runIdleTasks() ) {
      return G_SOURCE_CONTINUE;
    }
  }
  return G_SOURCE_REMOVE;
}


EventDispatcherPrivate::EventDispatcherPrivate ( GMainContext *ctx )
{
  _myThreadId = std::this_thread::get_id();

  //if we get a context specified ( usually when created for main thread ) we use it
  //otherwise we create our own
  if ( ctx ) {
    _ctx = ctx;
    g_main_context_ref ( _ctx );
  } else {
    _ctx = g_main_context_get_thread_default();
    if ( !_ctx ) {
      _ctx = g_main_context_new();
    } else {
      g_main_context_ref ( _ctx );
    }
  }
  g_main_context_push_thread_default( _ctx );

  _loop = g_main_loop_new( _ctx, false );

  _idleSource = g_idle_source_new ();
  g_source_set_callback ( _idleSource, eventLoopIdleFunc, this, nullptr );
}

EventDispatcherPrivate::~EventDispatcherPrivate()
{
  std::for_each ( _runningTimers.begin(), _runningTimers.end(), []( GLibTimerSource *src ){
    GLibTimerSource::destruct( src );
  });
  std::for_each ( _eventSources.begin(), _eventSources.end(), []( GAbstractEventSource *src ){
    GAbstractEventSource::destruct( src );
  });
  _runningTimers.clear();

  g_source_destroy( _idleSource );
  g_source_unref ( _idleSource );

  g_main_context_pop_thread_default( _ctx );
  g_main_context_unref( _ctx );
  g_main_loop_unref( _loop );
}

bool EventDispatcherPrivate::runIdleTasks()
{
  //run all user defined idle functions
  //if they return true, they are executed again in the next idle run
  decltype ( _idleFuncs ) rerunQueue;
  while ( _idleFuncs.size() ) {
    EventDispatcher::IdleFunction fun( std::move( _idleFuncs.front() ) );
    _idleFuncs.pop();
    if ( fun() )
      rerunQueue.push( std::move(fun) );
  }
  if ( !rerunQueue.empty() )
    _idleFuncs.swap( rerunQueue );


  //keep this as the last thing to call after all user code was executed
  if ( _unrefLater.size() )
    _unrefLater.clear();

  return _idleFuncs.size() || _unrefLater.size();
}

void EventDispatcherPrivate::enableIdleSource()
{
  if ( !_idleSource->context )
    g_source_attach ( _idleSource, _ctx );
}


EventDispatcher::EventDispatcher(void *ctx)
  : Base ( * new EventDispatcherPrivate( reinterpret_cast<GMainContext*>(ctx) ) )
{
  threadLocalDispatcher( this );
}

std::shared_ptr<EventDispatcher> EventDispatcher::createMain()
{
  return std::shared_ptr<EventDispatcher>( new EventDispatcher(g_main_context_default()) );
}

std::shared_ptr<EventDispatcher> EventDispatcher::createForThread()
{
  return std::shared_ptr<EventDispatcher>( new EventDispatcher() );
}

EventDispatcher::~EventDispatcher()
{
  *threadLocalDispatcher() = nullptr;
}

void EventDispatcher::updateEventSource( AbstractEventSource *notifier, int fd, int mode )
{
  Z_D();
  if ( notifier->eventDispatcher().lock().get() != this )
    ZYPP_THROW( zypp::Exception("Invalid event dispatcher used to update event source") );

  GAbstractEventSource *evSrc = nullptr;
  auto &evSrcList = d->_eventSources;
  auto itToEvSrc = std::find_if( evSrcList.begin(), evSrcList.end(), [ notifier ]( const auto elem ){ return elem->eventSource == notifier; } );
  if ( itToEvSrc == evSrcList.end() ) {

    evSrc = GAbstractEventSource::create( d );
    evSrc->eventSource = notifier;
    evSrcList.push_back( evSrc );

    g_source_attach( &evSrc->source, d->_ctx );

  } else
    evSrc = (*itToEvSrc);

  int cond = 0;
  if ( mode & AbstractEventSource::Read ) {
    cond = readMask() | G_IO_ERR;
  }
  if ( mode & AbstractEventSource::Write ) {
    cond = cond | writeMask() | G_IO_ERR;
  }
  if ( mode & AbstractEventSource::Exception ) {
    cond = cond | excpMask() | G_IO_ERR;
  }

  auto it = std::find_if( evSrc->pollfds.begin(), evSrc->pollfds.end(), [fd]( const auto &currPollFd ) {
    return currPollFd.pollfd == fd;
  });

  if ( it != evSrc->pollfds.end() ) {
    //found
    it->reqEvents = static_cast<GIOCondition>( cond );
    g_source_modify_unix_fd( &evSrc->source, it->tag, static_cast<GIOCondition>(cond) );
  } else {
    evSrc->pollfds.push_back(
      GUnixPollFD {
        static_cast<GIOCondition>(cond),
        fd,
        g_source_add_unix_fd( &evSrc->source, fd, static_cast<GIOCondition>(cond) )
      }
    );
  }
}

void EventDispatcher::removeEventSource( AbstractEventSource *notifier, int fd )
{
  Z_D();

  if ( notifier->eventDispatcher().lock().get() != this )
    ZYPP_THROW( zypp::Exception("Invalid event dispatcher used to remove event source") );

  auto &evList = d->_eventSources;
  auto it = std::find_if( evList.begin(), evList.end(), [ notifier ]( const auto elem ){ return elem->eventSource == notifier; } );

  if ( it == evList.end() )
    return;

  auto &fdList = (*it)->pollfds;

  if ( fd == -1 ) {
    //we clear out all unix_fd watches but do not destroy the source just yet. We currently might
    //be in the dispatch() function of that AbstractEventSource, make sure not to break the iterator
    //for the fd's
    for ( auto &pFD : fdList ) {
      if ( pFD.tag )
        g_source_remove_unix_fd( &(*it)->source, pFD.tag );
      pFD.pollfd = -1;
      pFD.tag = nullptr; //mark as orphaned, do not delete the element here this might break dipatching
    }
  } else {
    auto fdIt = std::find_if( fdList.begin(), fdList.end(), [ fd ]( const auto &pFd ){ return pFd.pollfd == fd; } );
    if ( fdIt != fdList.end() ) {
      if ( fdIt->tag )
        g_source_remove_unix_fd( &(*it)->source, (*fdIt).tag );
      //also do not remove here, mark as orphaned only to not break iterating in dispatch()
      fdIt->tag    = nullptr;
      fdIt->pollfd = -1;
    }
  }
}

void EventDispatcher::registerTimer( Timer *timer )
{
  Z_D();
  //make sure timer is not double registered
  for ( const GLibTimerSource *t : d->_runningTimers ) {
    if ( t->_t == timer )
      return;
  }

  GLibTimerSource *newSrc = GLibTimerSource::create();
  newSrc->_t = timer;
  d->_runningTimers.push_back( newSrc );

  g_source_attach( &newSrc->source, d->_ctx );
}

void EventDispatcher::removeTimer( Timer *timer )
{
  Z_D();
  auto it = std::find_if( d->_runningTimers.begin(), d->_runningTimers.end(), [ timer ]( const GLibTimerSource *src ){
    return src->_t == timer;
  });

  if ( it != d->_runningTimers.end() ) {
    GLibTimerSource *src = *it;
    d->_runningTimers.erase( it );
    GLibTimerSource::destruct( src );
  }
}

bool EventDispatcher::run_once()
{
  return g_main_context_iteration( d_func()->_ctx, false );
}

void EventDispatcher::run()
{
  g_main_loop_run( d_func()->_loop );
}

void EventDispatcher::quit()
{
  g_main_loop_quit( d_func()->_loop );
}

void EventDispatcher::invokeOnIdleImpl(EventDispatcher::IdleFunction &&callback)
{
  auto d = instance()->d_func();
  d->_idleFuncs.push( std::move(callback) );
  d->enableIdleSource();
}

void EventDispatcher::unrefLaterImpl(std::shared_ptr<void> &&ptr )
{
  Z_D();
  d->_unrefLater.push_back( std::forward< std::shared_ptr<void> >(ptr) );
  d->enableIdleSource();
}

ulong EventDispatcher::runningTimers() const
{
  return d_func()->_runningTimers.size();
}

std::shared_ptr<EventDispatcher> EventDispatcher::instance()
{
  auto ev = *threadLocalDispatcher();
  if ( ev )
    return ev->shared_this<EventDispatcher>();
  return std::shared_ptr<EventDispatcher>();
}

}
