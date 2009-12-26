/*
 * mixer.cpp - audio-device-independent mixer for LMMS
 *
 * Copyright (c) 2004-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * 
 * This file is part of Linux MultiMedia Studio - http://lmms.sourceforge.net
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */


#include <math.h>

#include "mixer.h"
#include "FxMixer.h"
#include "play_handle.h"
#include "song.h"
#include "templates.h"
#include "EnvelopeAndLfoParameters.h"
#include "note_play_handle.h"
#include "InstrumentTrack.h"
#include "debug.h"
#include "engine.h"
#include "config_mgr.h"
#include "sample_play_handle.h"
#include "piano_roll.h"
#include "MicroTimer.h"
#include "atomic_int.h"

// platform-specific audio-interface-classes
#include "AudioAlsa.h"
#include "AudioJack.h"
#include "AudioOss.h"
#include "AudioPortAudio.h"
#include "AudioPulseAudio.h"
#include "AudioSdl.h"
#include "AudioDummy.h"

// platform-specific midi-interface-classes
#include "MidiAlsaRaw.h"
#include "MidiAlsaSeq.h"
#include "MidiOss.h"
#include "MidiWinMM.h"
#include "MidiDummy.h"


static QVector<fx_ch_t> __fx_channel_jobs( NumFxChannels );



static void aligned_free( void * _buf )
{
	if( _buf != NULL )
	{
		int *ptr2=(int *)_buf - 1;
		_buf = (char *)_buf- *ptr2;
		free(_buf);
	}
}

static void * aligned_malloc( int _bytes )
{
	char *ptr,*ptr2,*aligned_ptr;
	int align_mask = ALIGN_SIZE- 1;
	ptr=(char *)malloc(_bytes +ALIGN_SIZE+ sizeof(int));
	if(ptr==NULL) return(NULL);

	ptr2 = ptr + sizeof(int);
	aligned_ptr = ptr2 + (ALIGN_SIZE- ((size_t)ptr2 & align_mask));


	ptr2 = aligned_ptr - sizeof(int);
	*((int *)ptr2)=(int)(aligned_ptr - ptr);

	return(aligned_ptr);
}



class MixerWorkerThread : public QThread
{
public:
	enum JobTypes
	{
		InvalidJob,
		PlayHandle,
		AudioPortEffects,
		EffectChannel,
		NumJobTypes
	} ;

	struct JobQueueItem
	{
		JobQueueItem() :
			type( InvalidJob ),
			job( NULL ),
			param( 0 ),
			done( false )
		{
		}
		JobQueueItem( JobTypes _type, void * _job, int _param = 0 ) :
			type( _type ),
			job( _job ),
			param( _param ),
			done( false )
		{
		}

		JobTypes type;
		void * job;
		int param;

		AtomicInt done;
	} ;


	struct JobQueue
	{
#define JOB_QUEUE_SIZE 1024
		JobQueue() :
			queueSize( 0 )
		{
		}

		JobQueueItem items[JOB_QUEUE_SIZE];
		int queueSize;
		AtomicInt itemsDone;
	} ;

	static JobQueue s_jobQueue;

	MixerWorkerThread( int _worker_num, mixer * _mixer ) :
		QThread( _mixer ),
		m_workingBuf( (sampleFrame *) aligned_malloc(
					_mixer->framesPerPeriod() *
						sizeof( sampleFrame ) ) ),
		m_workerNum( _worker_num ),
		m_quit( false ),
		m_mixer( _mixer ),
		m_queueReadyWaitCond( &m_mixer->m_queueReadyWaitCond )
	{
	}

	virtual ~MixerWorkerThread()
	{
		aligned_free( m_workingBuf );
	}

	virtual void quit()
	{
		m_quit = true;
	}

	void processJobQueue();


private:
	virtual void run()
	{
#if 0
#ifdef LMMS_BUILD_LINUX
#ifdef LMMS_HAVE_SCHED_H
		cpu_set_t mask;
		CPU_ZERO( &mask );
		CPU_SET( m_workerNum, &mask );
		sched_setaffinity( 0, sizeof( mask ), &mask );
#endif
#endif
#endif
		QMutex m;
		while( m_quit == false )
		{
			m.lock();
			m_queueReadyWaitCond->wait( &m );
			processJobQueue();
			m.unlock();
		}
	}

	sampleFrame * m_workingBuf;
	int m_workerNum;
	volatile bool m_quit;
	mixer * m_mixer;
	QWaitCondition * m_queueReadyWaitCond;

} ;


MixerWorkerThread::JobQueue MixerWorkerThread::s_jobQueue;



void MixerWorkerThread::processJobQueue()
{
	for( int i = 0; i < s_jobQueue.queueSize; ++i )
	{
		JobQueueItem * it = &s_jobQueue.items[i];
		if( it->done.fetchAndStoreOrdered( 1 ) == 0 )
		{
			switch( it->type )
			{
				case PlayHandle:
					( (playHandle *) it->job )->
							play( m_workingBuf );
					break;
				case AudioPortEffects:
					{
	AudioPort * a = (AudioPort *) it->job;
	const bool me = a->processEffects();
	if( me || a->m_bufferUsage != AudioPort::NoUsage )
	{
		engine::fxMixer()->mixToChannel( a->firstBuffer(),
							a->nextFxChannel() );
		a->nextPeriod();
	}
					}
					break;
				case EffectChannel:
	engine::fxMixer()->processChannel( (fx_ch_t) it->param );
					break;
				default:
					break;
			}
			s_jobQueue.itemsDone.fetchAndAddOrdered( 1 );
		}
	}
}

#define FILL_JOB_QUEUE_BEGIN(_vec_type,_vec,_condition)			\
	MixerWorkerThread::s_jobQueue.queueSize = 0;			\
	MixerWorkerThread::s_jobQueue.itemsDone = 0;			\
	for( _vec_type::Iterator it = _vec.begin();			\
					it != _vec.end(); ++it )	\
	{								\
		if( _condition )					\
		{

#define FILL_JOB_QUEUE_END()						\
			++MixerWorkerThread::s_jobQueue.queueSize;	\
		}							\
	}

#define FILL_JOB_QUEUE(_vec_type,_vec,_job_type,_condition)		\
	FILL_JOB_QUEUE_BEGIN(_vec_type,_vec,_condition)			\
	MixerWorkerThread::s_jobQueue.items				\
		[MixerWorkerThread::s_jobQueue.queueSize] =		\
			MixerWorkerThread::JobQueueItem( _job_type,	\
							(void *) *it );	\
	FILL_JOB_QUEUE_END()

#define FILL_JOB_QUEUE_PARAM(_vec_type,_vec,_job_type,_condition)	\
	FILL_JOB_QUEUE_BEGIN(_vec_type,_vec,_condition)			\
	MixerWorkerThread::s_jobQueue.items				\
		[MixerWorkerThread::s_jobQueue.queueSize] =		\
			MixerWorkerThread::JobQueueItem( _job_type,	\
							NULL, *it );	\
	FILL_JOB_QUEUE_END()

#define START_JOBS()							\
	m_queueReadyWaitCond.wakeAll();

// define a pause instruction for spinlock-loop - merely useful on
// HyperThreading systems with just one physical core (e.g. Intel Atom)
#ifdef LMMS_HOST_X86
#define SPINLOCK_PAUSE()        asm( "pause" )
#else
#ifdef LMMS_HOST_X86_64
#define SPINLOCK_PAUSE()        asm( "pause" )
#else
#define SPINLOCK_PAUSE()
#endif
#endif

#define WAIT_FOR_JOBS()							\
	m_workers[m_numWorkers]->processJobQueue();			\
	while( MixerWorkerThread::s_jobQueue.itemsDone <		\
			MixerWorkerThread::s_jobQueue.queueSize )	\
	{								\
		SPINLOCK_PAUSE();					\
	}								\




mixer::mixer() :
	m_framesPerPeriod( DEFAULT_BUFFER_SIZE ),
	m_workingBuf( NULL ),
	m_inputBufferRead( 0 ),
	m_inputBufferWrite( 1 ),
	m_readBuf( NULL ),
	m_writeBuf( NULL ),
	m_cpuLoad( 0 ),
	m_workers(),
	m_numWorkers( QThread::idealThreadCount()-1 ),
	m_queueReadyWaitCond(),
	m_qualitySettings( qualitySettings::Mode_Draft ),
	m_masterGain( 1.0f ),
	m_audioDev( NULL ),
	m_oldAudioDev( NULL ),
	m_globalMutex( QMutex::Recursive )
{
	for( int i = 0; i < 2; ++i )
	{
		m_inputBufferFrames[i] = 0;
		m_inputBufferSize[i] = DEFAULT_BUFFER_SIZE * 100;
		m_inputBuffer[i] = new sampleFrame[ DEFAULT_BUFFER_SIZE * 100 ];
		clearAudioBuffer( m_inputBuffer[i], m_inputBufferSize[i] );
	}

	for( int i = 1; i < NumFxChannels+1; ++i )
	{
		__fx_channel_jobs[i-1] = (fx_ch_t) i;
	}

	// just rendering?
	if( !engine::hasGUI() )
	{
		m_framesPerPeriod = DEFAULT_BUFFER_SIZE;
		m_fifo = new fifo( 1 );
	}
	else if( configManager::inst()->value( "mixer", "framesperaudiobuffer"
						).toInt() >= 32 )
	{
		m_framesPerPeriod =
			(fpp_t) configManager::inst()->value( "mixer",
					"framesperaudiobuffer" ).toInt();

		if( m_framesPerPeriod > DEFAULT_BUFFER_SIZE )
		{
			m_fifo = new fifo( m_framesPerPeriod
							/ DEFAULT_BUFFER_SIZE );
			m_framesPerPeriod = DEFAULT_BUFFER_SIZE;
		}
		else
		{
			m_fifo = new fifo( 1 );
		}
	}
	else
	{
		configManager::inst()->setValue( "mixer",
							"framesperaudiobuffer",
				QString::number( m_framesPerPeriod ) );
		m_fifo = new fifo( 1 );
	}

	m_workingBuf = (sampleFrame*) aligned_malloc( m_framesPerPeriod *
							sizeof( sampleFrame ) );
	for( Uint8 i = 0; i < 3; i++ )
	{
		m_readBuf = (surroundSampleFrame*)
			aligned_malloc( m_framesPerPeriod *
						sizeof( surroundSampleFrame ) );

		clearAudioBuffer( m_readBuf, m_framesPerPeriod );
		m_bufferPool.push_back( m_readBuf );
	}

	for( int i = 0; i < m_numWorkers+1; ++i )
	{
		MixerWorkerThread * wt = new MixerWorkerThread( i, this );
		if( i < m_numWorkers )
		{
			wt->start( QThread::TimeCriticalPriority );
		}
		m_workers.push_back( wt );
	}

	m_poolDepth = 2;
	m_readBuffer = 0;
	m_writeBuffer = 1;
}




mixer::~mixer()
{
	// distribute an empty job-queue so that worker-threads
	// get out of their processing-loop
	MixerWorkerThread::s_jobQueue.queueSize = 0;
	for( int w = 0; w < m_numWorkers; ++w )
	{
		m_workers[w]->quit();
	}
	START_JOBS();
	for( int w = 0; w < m_numWorkers; ++w )
	{
		m_workers[w]->wait( 500 );
	}

	while( m_fifo->available() )
	{
		delete[] m_fifo->read();
	}
	delete m_fifo;

	delete m_audioDev;
	delete m_midiClient;

	for( Uint8 i = 0; i < 3; i++ )
	{
		aligned_free( m_bufferPool[i] );
	}

	aligned_free( m_workingBuf );
}




void mixer::initDevices()
{
	m_audioDev = tryAudioDevices();
	m_midiClient = tryMidiClients();
}




void mixer::startProcessing( bool _needs_fifo )
{
	if( _needs_fifo )
	{
		m_fifoWriter = new fifoWriter( this, m_fifo );
		m_fifoWriter->start( QThread::HighPriority );
	}
	else
	{
		m_fifoWriter = NULL;
	}

	m_audioDev->startProcessing();
}




void mixer::stopProcessing()
{
	if( m_fifoWriter != NULL )
	{
		m_fifoWriter->finish();
		m_audioDev->stopProcessing();
		m_fifoWriter->wait( 1000 );
		m_fifoWriter->terminate();
		delete m_fifoWriter;
		m_fifoWriter = NULL;
	}
	else
	{
		m_audioDev->stopProcessing();
	}
}




sample_rate_t mixer::baseSampleRate() const
{
	sample_rate_t sr =
		configManager::inst()->value( "mixer", "samplerate" ).toInt();
	if( sr < 44100 )
	{
		sr = 44100;
	}
	return sr;
}




sample_rate_t mixer::outputSampleRate() const
{
	return m_audioDev != NULL ? m_audioDev->sampleRate() :
							baseSampleRate();
}




sample_rate_t mixer::inputSampleRate() const
{
	return m_audioDev != NULL ? m_audioDev->sampleRate() :
							baseSampleRate();
}




sample_rate_t mixer::processingSampleRate() const
{
	return outputSampleRate() * m_qualitySettings.sampleRateMultiplier();
}




bool mixer::criticalXRuns() const
{
	return m_cpuLoad >= 99 && engine::getSong()->realTimeTask() == true;
}




void mixer::pushInputFrames( sampleFrame * _ab, const f_cnt_t _frames )
{
	lockInputFrames();

	f_cnt_t frames = m_inputBufferFrames[ m_inputBufferWrite ];
	int size = m_inputBufferSize[ m_inputBufferWrite ];
	sampleFrame * buf = m_inputBuffer[ m_inputBufferWrite ];
	
	if( frames + _frames > size )
	{
		size = qMax( size * 2, frames + _frames );
		sampleFrame * ab = new sampleFrame[ size ];
		memcpy( ab, buf, frames * sizeof( sampleFrame ) );
		delete [] buf;

		m_inputBufferSize[ m_inputBufferWrite ] = size;
		m_inputBuffer[ m_inputBufferWrite ] = ab;

		buf = ab;
	}
	
	memcpy( &buf[ frames ], _ab, _frames * sizeof( sampleFrame ) );
	m_inputBufferFrames[ m_inputBufferWrite ] += _frames;
	
	unlockInputFrames();
}




const surroundSampleFrame * mixer::renderNextBuffer()
{
	MicroTimer timer;
	static song::playPos last_metro_pos = -1;

	song::playPos p = engine::getSong()->getPlayPos(
						song::Mode_PlayPattern );
	if( engine::getSong()->playMode() == song::Mode_PlayPattern &&
		engine::getPianoRoll()->isRecording() == true &&
		p != last_metro_pos && p.getTicks() %
					(DefaultTicksPerTact / 4 ) == 0 )
	{
		addPlayHandle( new samplePlayHandle( "misc/metronome01.ogg" ) );
		last_metro_pos = p;
	}

	lockInputFrames();
	// swap buffer
	m_inputBufferWrite = ( m_inputBufferWrite + 1 ) % 2;
	m_inputBufferRead =  ( m_inputBufferRead + 1 ) % 2;
	// clear new write buffer
	m_inputBufferFrames[ m_inputBufferWrite ] = 0;
	unlockInputFrames();


	// now we have to make sure no other thread does anything bad
	// while we're acting...
	lock();

	// remove all play-handles that have to be deleted and delete
	// them if they still exist...
	// maybe this algorithm could be optimized...
	ConstPlayHandleList::Iterator it_rem = m_playHandlesToRemove.begin();
	while( it_rem != m_playHandlesToRemove.end() )
	{
		PlayHandleList::Iterator it = qFind( m_playHandles.begin(),
						m_playHandles.end(), *it_rem );

		if( it != m_playHandles.end() )
		{
			delete *it;
			m_playHandles.erase( it );
		}

		it_rem = m_playHandlesToRemove.erase( it_rem );
	}

	// rotate buffers
	m_writeBuffer = ( m_writeBuffer + 1 ) % m_poolDepth;
	m_readBuffer = ( m_readBuffer + 1 ) % m_poolDepth;

	m_writeBuf = m_bufferPool[m_writeBuffer];
	m_readBuf = m_bufferPool[m_readBuffer];

	// clear last audio-buffer
	clearAudioBuffer( m_writeBuf, m_framesPerPeriod );

	// prepare master mix (clear internal buffers etc.)
	engine::fxMixer()->prepareMasterMix();

	// create play-handles for new notes, samples etc.
	engine::getSong()->processNextBuffer();


	// STAGE 1: run and render all play handles
	FILL_JOB_QUEUE(PlayHandleList,m_playHandles,
					MixerWorkerThread::PlayHandle,
					!( *it )->done());
	START_JOBS();
	WAIT_FOR_JOBS();

	// removed all play handles which are done
	for( PlayHandleList::Iterator it = m_playHandles.begin();
						it != m_playHandles.end(); )
	{
		if( ( *it )->affinityMatters() &&
			( *it )->affinity() != QThread::currentThread() )
		{
			++it;
			continue;
		}
		if( ( *it )->done() )
		{
			delete *it;
			it = m_playHandles.erase( it );
		}
		else
		{
			++it;
		}
	}


	// STAGE 2: process effects of all instrument- and sampletracks
	FILL_JOB_QUEUE(QVector<AudioPort*>,m_audioPorts,
					MixerWorkerThread::AudioPortEffects,1);
	START_JOBS();
	WAIT_FOR_JOBS();


	// STAGE 3: process effects in FX mixer
	FILL_JOB_QUEUE_PARAM(QVector<fx_ch_t>,__fx_channel_jobs,
					MixerWorkerThread::EffectChannel,1);
	START_JOBS();
	WAIT_FOR_JOBS();


	// STAGE 4: do master mix in FX mixer
	engine::fxMixer()->masterMix( m_writeBuf );

	unlock();


	emit nextAudioBuffer();

	// and trigger LFOs
	EnvelopeAndLfoParameters::triggerLfo();
	Controller::triggerFrameCounter();

	const float new_cpu_load = timer.elapsed() / 10000.0f *
				processingSampleRate() / m_framesPerPeriod;
	m_cpuLoad = tLimit( (int) ( new_cpu_load * 0.1f + m_cpuLoad * 0.9f ), 0,
									100 );

	return m_readBuf;
}




// removes all play-handles. this is neccessary, when the song is stopped ->
// all remaining notes etc. would be played until their end
void mixer::clear()
{
	// TODO: m_midiClient->noteOffAll();
	lock();
	for( PlayHandleList::Iterator it = m_playHandles.begin();
					it != m_playHandles.end(); ++it )
	{
		// we must not delete instrument-play-handles as they exist
		// during the whole lifetime of an instrument
		if( ( *it )->type() != playHandle::InstrumentPlayHandle )
		{
			m_playHandlesToRemove.push_back( *it );
		}
	}
	unlock();
}




void mixer::bufferToPort( const sampleFrame * _buf,
					const fpp_t _frames,
					const f_cnt_t _offset,
					stereoVolumeVector _vv,
						AudioPort * _port )
{
	const int start_frame = _offset % m_framesPerPeriod;
	int end_frame = start_frame + _frames;
	const int loop1_frame = qMin<int>( end_frame, m_framesPerPeriod );

	_port->lockFirstBuffer();
	sampleFrame * obuf = _port->firstBuffer()+start_frame;
	for( int frame = 0; frame < loop1_frame-start_frame; ++frame )
	{
		obuf[frame][0] += _buf[frame][0] * _vv.vol[0];
		obuf[frame][1] += _buf[frame][1] * _vv.vol[1];
	}
	_port->unlockFirstBuffer();

	_port->lockSecondBuffer();
	if( end_frame > m_framesPerPeriod )
	{
		const int frames_done = m_framesPerPeriod - start_frame;
		end_frame -= m_framesPerPeriod;
		end_frame = qMin<int>( end_frame, m_framesPerPeriod );
		sampleFrame * obuf = _port->secondBuffer();
		for( fpp_t frame = 0; frame < end_frame; ++frame )
		{
			obuf[frame][0] += _buf[frames_done + frame][0] *
								_vv.vol[0];
			obuf[frame][1] += _buf[frames_done + frame][1] *
								_vv.vol[1];
		}
		// we used both buffers so set flags
		_port->m_bufferUsage = AudioPort::BothBuffers;
	}
	else if( _port->m_bufferUsage == AudioPort::NoUsage )
	{
		// only first buffer touched
		_port->m_bufferUsage = AudioPort::FirstBuffer;
	}
	_port->unlockSecondBuffer();
}




void mixer::clearAudioBuffer( sampleFrame * _ab, const f_cnt_t _frames,
							const f_cnt_t _offset )
{
	memset( _ab+_offset, 0, sizeof( *_ab ) * _frames );
}



#ifndef LMMS_DISABLE_SURROUND
void mixer::clearAudioBuffer( surroundSampleFrame * _ab, const f_cnt_t _frames,
							const f_cnt_t _offset )
{
	memset( _ab+_offset, 0, sizeof( *_ab ) * _frames );
}
#endif




float mixer::peakValueLeft( sampleFrame * _ab, const f_cnt_t _frames )
{
	float p = 0.0f;
	for( f_cnt_t f = 0; f < _frames; ++f )
	{
		if( _ab[f][0] > p )
		{
			p = _ab[f][0];
		}
		else if( -_ab[f][0] > p )
		{
			p = -_ab[f][0];
		}
	}
	return p;
}




float mixer::peakValueRight( sampleFrame * _ab, const f_cnt_t _frames )
{
	float p = 0.0f;
	for( f_cnt_t f = 0; f < _frames; ++f )
	{
		if( _ab[f][1] > p )
		{
			p = _ab[f][1];
		}
		else if( -_ab[f][1] > p )
		{
			p = -_ab[f][1];
		}
	}
	return p;
}




void mixer::changeQuality( const struct qualitySettings & _qs )
{
	// don't delete the audio-device
	stopProcessing();

	m_qualitySettings = _qs;
	m_audioDev->applyQualitySettings();

	emit sampleRateChanged();
	emit qualitySettingsChanged();

	startProcessing();
}




void mixer::setAudioDevice( AudioDevice * _dev )
{
	stopProcessing();

	m_oldAudioDev = m_audioDev;

	if( _dev == NULL )
	{
		printf( "param _dev == NULL in mixer::setAudioDevice(...). "
					"Trying any working audio-device\n" );
		m_audioDev = tryAudioDevices();
	}
	else
	{
		m_audioDev = _dev;
	}

	emit sampleRateChanged();

	startProcessing();
}




void mixer::setAudioDevice( AudioDevice * _dev,
				const struct qualitySettings & _qs,
				bool _needs_fifo )
{
	// don't delete the audio-device
	stopProcessing();

	m_qualitySettings = _qs;
	m_oldAudioDev = m_audioDev;

	if( _dev == NULL )
	{
		printf( "param _dev == NULL in mixer::setAudioDevice(...). "
					"Trying any working audio-device\n" );
		m_audioDev = tryAudioDevices();
	}
	else
	{
		m_audioDev = _dev;
	}

	emit qualitySettingsChanged();
	emit sampleRateChanged();

	startProcessing( _needs_fifo );
}




void mixer::restoreAudioDevice()
{
	if( m_oldAudioDev != NULL )
	{
		stopProcessing();
		delete m_audioDev;

		m_audioDev = m_oldAudioDev;
		emit sampleRateChanged();

		m_oldAudioDev = NULL;
		startProcessing();
	}
}




void mixer::removeAudioPort( AudioPort * _port )
{
	QVector<AudioPort *>::Iterator it = qFind( m_audioPorts.begin(),
							m_audioPorts.end(),
							_port );
	if( it != m_audioPorts.end() )
	{
		lock();
		m_audioPorts.erase( it );
		unlock();
	}
}




void mixer::removePlayHandle( playHandle * _ph )
{
	lock();
	// check thread affinity as we must not delete play-handles
	// which were created in a thread different than mixer thread
	if( _ph->affinityMatters() &&
				_ph->affinity() == QThread::currentThread() )
	{
		PlayHandleList::Iterator it =
				qFind( m_playHandles.begin(),
						m_playHandles.end(), _ph );
		if( it != m_playHandles.end() )
		{
			m_playHandles.erase( it );
			delete _ph;
		}
	}
	else
	{
		m_playHandlesToRemove.push_back( _ph );
	}
	unlock();
}




void mixer::removePlayHandles( track * _track )
{
	lock();
	PlayHandleList::Iterator it = m_playHandles.begin();
	while( it != m_playHandles.end() )
	{
		if( ( *it )->isFromTrack( _track ) )
		{
			delete *it;
			it = m_playHandles.erase( it );
		}
		else
		{
			++it;
		}
	}
	unlock();
}




AudioDevice * mixer::tryAudioDevices()
{
	bool success_ful = false;
	AudioDevice * dev = NULL;
	QString dev_name = configManager::inst()->value( "mixer", "audiodev" );

	if( dev_name == AudioDummy::name() )
	{
		dev_name = "";
	}

#ifdef LMMS_HAVE_ALSA
	if( dev_name == AudioAlsa::name() || dev_name == "" )
	{
		dev = new AudioAlsa( success_ful, this );
		if( success_ful )
		{
			m_audioDevName = AudioAlsa::name();
			return dev;
		}
		delete dev;
	}
#endif


#ifdef LMMS_HAVE_PORTAUDIO
	if( dev_name == AudioPortAudio::name() || dev_name == "" )
	{
		dev = new AudioPortAudio( success_ful, this );
		if( success_ful )
		{
			m_audioDevName = AudioPortAudio::name();
			return dev;
		}
		delete dev;
	}
#endif


#ifdef LMMS_HAVE_PULSEAUDIO
	if( dev_name == AudioPulseAudio::name() || dev_name == "" )
	{
		dev = new AudioPulseAudio( success_ful, this );
		if( success_ful )
		{
			m_audioDevName = AudioPulseAudio::name();
			return dev;
		}
		delete dev;
	}
#endif


#ifdef LMMS_HAVE_OSS
	if( dev_name == AudioOss::name() || dev_name == "" )
	{
		dev = new AudioOss( success_ful, this );
		if( success_ful )
		{
			m_audioDevName = AudioOss::name();
			return dev;
		}
		delete dev;
	}
#endif


#ifdef LMMS_HAVE_JACK
	if( dev_name == AudioJack::name() || dev_name == "" )
	{
		dev = new AudioJack( success_ful, this );
		if( success_ful )
		{
			m_audioDevName = AudioJack::name();
			return dev;
		}
		delete dev;
	}
#endif


#ifdef LMMS_HAVE_SDL
	if( dev_name == AudioSdl::name() || dev_name == "" )
	{
		dev = new AudioSdl( success_ful, this );
		if( success_ful )
		{
			m_audioDevName = AudioSdl::name();
			return dev;
		}
		delete dev;
	}
#endif

	// add more device-classes here...
	//dev = new audioXXXX( SAMPLE_RATES[m_qualityLevel], success_ful, this );
	//if( sucess_ful )
	//{
	//	return dev;
	//}
	//delete dev

	printf( "No audio-driver working - falling back to dummy-audio-"
		"driver\nYou can render your songs and listen to the output "
		"files...\n" );

	m_audioDevName = AudioDummy::name();

	return new AudioDummy( success_ful, this );
}




MidiClient * mixer::tryMidiClients()
{
	QString client_name = configManager::inst()->value( "mixer",
								"mididev" );

#ifdef LMMS_HAVE_ALSA
	if( client_name == MidiAlsaSeq::name() || client_name == "" )
	{
		MidiAlsaSeq * malsas = new MidiAlsaSeq;
		if( malsas->isRunning() )
		{
			m_midiClientName = MidiAlsaSeq::name();
			return malsas;
		}
		delete malsas;
	}

	if( client_name == MidiAlsaRaw::name() || client_name == "" )
	{
		MidiAlsaRaw * malsar = new MidiAlsaRaw;
		if( malsar->isRunning() )
		{
			m_midiClientName = MidiAlsaRaw::name();
			return malsar;
		}
		delete malsar;
	}
#endif

#ifdef LMMS_HAVE_OSS
	if( client_name == MidiOss::name() || client_name == "" )
	{
		MidiOss * moss = new MidiOss;
		if( moss->isRunning() )
		{
			m_midiClientName = MidiOss::name();
			return moss;
		}
		delete moss;
	}
#endif

#ifdef LMMS_BUILD_WIN32
	if( client_name == MidiWinMM::name() || client_name == "" )
	{
		MidiWinMM * mwmm = new MidiWinMM;
//		if( moss->isRunning() )
		{
			m_midiClientName = MidiWinMM::name();
			return mwmm;
		}
		delete mwmm;
	}
#endif

	printf( "Couldn't create MIDI-client, neither with ALSA nor with "
		"OSS. Will use dummy-MIDI-client.\n" );

	m_midiClientName = MidiDummy::name();

	return new MidiDummy;
}









mixer::fifoWriter::fifoWriter( mixer * _mixer, fifo * _fifo ) :
	m_mixer( _mixer ),
	m_fifo( _fifo ),
	m_writing( true )
{
}




void mixer::fifoWriter::finish()
{
	m_writing = false;
}




void mixer::fifoWriter::run()
{
#if 0
#ifdef LMMS_BUILD_LINUX
#ifdef LMMS_HAVE_SCHED_H
	cpu_set_t mask;
	CPU_ZERO( &mask );
	CPU_SET( 0, &mask );
	sched_setaffinity( 0, sizeof( mask ), &mask );
#endif
#endif
#endif

	const fpp_t frames = m_mixer->framesPerPeriod();
	while( m_writing )
	{
		surroundSampleFrame * buffer = new surroundSampleFrame[frames];
		const surroundSampleFrame * b = m_mixer->renderNextBuffer();
		memcpy( buffer, b, frames * sizeof( surroundSampleFrame ) );
		m_fifo->write( buffer );
	}

	m_fifo->write( NULL );
}




#include "moc_mixer.cxx"
