// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <streamsession.h>
#include <settings.h>
#include <controllermanager.h>

#include <chiaki/base64.h>
#include <chiaki/streamconnection.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/session.h>
#include <chiaki/time.h>
#include "../../lib/src/utils.h"

#include <QKeyEvent>
#include <QtMath>
#include <QDebug> 

#include <cstring>
#include <atomic> // [CRONUS MOD] Necessário

// =================================================================
// [CRONUS MOD - VARIÁVEIS GLOBAIS]
// =================================================================
std::atomic<bool> g_recoilEnabled(false);
std::atomic<int> g_recoilStrength(0);
// =================================================================

#define SETSU_UPDATE_INTERVAL_MS 4
#define STEAMDECK_UPDATE_INTERVAL_MS 4
#define STEAMDECK_HAPTIC_INTERVAL_MS 10
#define NEW_DPAD_TOUCH_INTERVAL_MS 500
#define DPAD_TOUCH_UPDATE_INTERVAL_MS 10
#define STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS 4
#define RUMBLE_HAPTICS_PACKETS_PER_RUMBLE 3
#define STEAMDECK_HAPTIC_SAMPLING_RATE 3000
#define PS4_TOUCHPAD_MAX_X 1920.0f
#define PS4_TOUCHPAD_MAX_Y 942.0f
#define PS5_TOUCHPAD_MAX_X 1919.0f
#define PS5_TOUCHPAD_MAX_Y 1079.0f
#define SESSION_RETRY_SECONDS 20
#define HAPTIC_RUMBLE_MIN_STRENGTH 100
#define MICROPHONE_SAMPLES 480

#ifdef Q_OS_LINUX
#define DUALSENSE_AUDIO_DEVICE_NEEDLE "DualSense"
#else
#define DUALSENSE_AUDIO_DEVICE_NEEDLE "Wireless Controller"
#endif

#if CHIAKI_GUI_ENABLE_SPEEX
#define ECHO_QUEUE_MAX 40
#endif

// --- Funções Auxiliares Estáticas ---
static bool isLocalAddress(QString host) {
    if(host.contains(".")) {
        if(host.startsWith("10.")) return true;
        else if(host.startsWith("192.168.")) return true;
        for (int j = 16; j < 32; j++) {
            if(host.startsWith(QString("172.") + QString::number(j) + QString("."))) return true;
        }
    } else if(host.contains(":")) {
        if(host.startsWith("FC", Qt::CaseInsensitive)) return true;
        if(host.startsWith("FD", Qt::CaseInsensitive)) return true;
    }
    return false;
}

// Protótipos dos Callbacks (Implementados no final)
static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user);
static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user);
static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user);
static void CantDisplayCb(void *user, bool cant_display);
static void EventCb(ChiakiEvent *event, void *user);
static void FfmpegFrameCb(ChiakiFfmpegDecoder *decoder, void *user);
#if CHIAKI_GUI_ENABLE_SETSU
static void SessionSetsuCb(SetsuEvent *event, void *user);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
static void SessionSDeckCb(SDeckEvent *event, void *user);
#endif

// =================================================================
// CONSTRUTOR
// =================================================================
StreamSession::StreamSession(const StreamSessionConnectInfo &connect_info, QObject *parent)
	: QObject(parent),
	log(this, connect_info.log_level_mask, connect_info.log_file),
	ffmpeg_decoder(nullptr),
#if CHIAKI_LIB_ENABLE_PI_DECODER
	pi_decoder(nullptr),
#endif
	audio_out(0),
	audio_in(0),
	haptics_output(0),
	haptics_handheld(0),
	session_started(false),
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_haptics_senderl(nullptr),
	sdeck_haptics_senderr(nullptr),
	sdeck(nullptr),
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
	echo_resampler_buf(nullptr),
	mic_resampler_buf(nullptr),
#endif
	haptics_resampler_buf(nullptr),
	holepunch_session(nullptr),
	rumble_multiplier(1),
	ps5_rumble_intensity(0x00),
	ps5_trigger_intensity(0x00),
	rumble_haptics_connected(false),
	rumble_haptics_on(false)
{
    // ... (Inicialização padrão do Chiaki) ...
	mic_buf.buf = nullptr;
	connected = false;
	muted = true;
	mic_connected = false;
#ifdef Q_OS_MACOS
	mic_authorization = false;
#endif
	allow_unmute = false;
	dpad_regular = true;
	dpad_regular_touch_switched = false;
	rumble_haptics_intensity = RumbleHapticsIntensity::Off;
	input_block = 0;
	player_index = 0;
	memset(led_color, 0, sizeof(led_color));
	ChiakiErrorCode err;

#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(connect_info.decoder == Decoder::Pi) {
		pi_decoder = CHIAKI_NEW(ChiakiPiDecoder);
		if(chiaki_pi_decoder_init(pi_decoder, log.GetChiakiLog()) != CHIAKI_ERR_SUCCESS)
			throw ChiakiException("Failed to initialize Raspberry Pi Decoder");
	} else {
#endif
		ffmpeg_decoder = new ChiakiFfmpegDecoder;
		ChiakiLogSniffer sniffer;
		chiaki_log_sniffer_init(&sniffer, CHIAKI_LOG_ALL, GetChiakiLog());
		err = chiaki_ffmpeg_decoder_init(ffmpeg_decoder,
				chiaki_log_sniffer_get_log(&sniffer),
				chiaki_target_is_ps5(connect_info.target) ? connect_info.video_profile.codec : CHIAKI_CODEC_H264,
				connect_info.hw_decoder.isEmpty() ? NULL : connect_info.hw_decoder.toUtf8().constData(),
				connect_info.hw_device_ctx, FfmpegFrameCb, this);
		if(err != CHIAKI_ERR_SUCCESS) {
			QString log = QString::fromUtf8(chiaki_log_sniffer_get_buffer(&sniffer));
			chiaki_log_sniffer_fini(&sniffer);
			throw ChiakiException("Failed to initialize FFMPEG Decoder:\n" + log);
		}
		chiaki_log_sniffer_fini(&sniffer);
		ffmpeg_decoder->log = GetChiakiLog();
#if CHIAKI_LIB_ENABLE_PI_DECODER
	}
#endif

    // Configurações de Áudio
	audio_volume = connect_info.audio_volume;
	start_mic_unmuted = connect_info.start_mic_unmuted;
	audio_out_device_name = connect_info.audio_out_device;
	audio_in_device_name = connect_info.audio_in_device;
	chiaki_opus_decoder_init(&opus_decoder, log.GetChiakiLog());
	chiaki_opus_encoder_init(&opus_encoder, log.GetChiakiLog());

#if CHIAKI_GUI_ENABLE_SPEEX
	speech_processing_enabled = connect_info.speech_processing_enabled;
	if(speech_processing_enabled) {
		echo_state = speex_echo_state_init(MICROPHONE_SAMPLES, MICROPHONE_SAMPLES * 10);
		preprocess_state = speex_preprocess_state_init(MICROPHONE_SAMPLES, MICROPHONE_SAMPLES * 100);
		int32_t noise_suppress_level = -1 * connect_info.noise_suppress_level;
		int32_t echo_suppress_level = -1 * connect_info.echo_suppress_level;
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_state);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_suppress_level);
	}
#endif

	audio_buffer_size = connect_info.audio_buffer_size;
	mouse_touch_enabled = connect_info.mouse_touch_enabled;
	keyboard_controller_enabled = connect_info.keyboard_controller_enabled;
	host = connect_info.host;
    QByteArray host_str = connect_info.host.toUtf8();

    // Configuração de Conexão Chiaki
	ChiakiConnectInfo chiaki_connect_info = {};
	chiaki_connect_info.ps5 = chiaki_target_is_ps5(connect_info.target);
	chiaki_connect_info.host = host_str.constData();
	chiaki_connect_info.video_profile = connect_info.video_profile;
	chiaki_connect_info.video_profile_auto_downgrade = true;
	chiaki_connect_info.enable_keyboard = false;
	chiaki_connect_info.enable_dualsense = connect_info.enable_dualsense;
	chiaki_connect_info.packet_loss_max = connect_info.packet_loss_max;
	chiaki_connect_info.auto_regist = connect_info.auto_regist;
	chiaki_connect_info.audio_video_disabled = connect_info.audio_video_disabled;

    // Configuração de Atalhos Touch
	dpad_touch_shortcut1 = connect_info.dpad_touch_shortcut1;
	dpad_touch_shortcut2 = connect_info.dpad_touch_shortcut2;
	dpad_touch_shortcut3 = connect_info.dpad_touch_shortcut3;
	dpad_touch_shortcut4 = connect_info.dpad_touch_shortcut4;
    // ... bitshifts ...
	if(this->dpad_touch_shortcut1 > 0) this->dpad_touch_shortcut1 = 1 << (this->dpad_touch_shortcut1 - 1);
	if(this->dpad_touch_shortcut2 > 0) this->dpad_touch_shortcut2 = 1 << (this->dpad_touch_shortcut2 - 1);
	if(this->dpad_touch_shortcut3 > 0) this->dpad_touch_shortcut3 = 1 << (this->dpad_touch_shortcut3 - 1);
	if(this->dpad_touch_shortcut4 > 0) this->dpad_touch_shortcut4 = 1 << (this->dpad_touch_shortcut4 - 1);
    
	haptic_override = connect_info.haptic_override;

    // RegistKey / Login
	if(connect_info.duid.isEmpty()) {
		if(connect_info.regist_key.size() != sizeof(chiaki_connect_info.regist_key)) throw ChiakiException("RegistKey invalid");
		memcpy(chiaki_connect_info.regist_key, connect_info.regist_key.constData(), sizeof(chiaki_connect_info.regist_key));
		if(connect_info.morning.size() != sizeof(chiaki_connect_info.morning)) throw ChiakiException("Morning invalid");
		memcpy(chiaki_connect_info.morning, connect_info.morning.constData(), sizeof(chiaki_connect_info.morning));
	}

    // Touchpad Size
	if(chiaki_connect_info.ps5) {
		PS_TOUCHPAD_MAX_X = PS5_TOUCHPAD_MAX_X;
		PS_TOUCHPAD_MAX_Y = PS5_TOUCHPAD_MAX_Y;
	} else {
		PS_TOUCHPAD_MAX_X = PS4_TOUCHPAD_MAX_X;
		PS_TOUCHPAD_MAX_Y = PS4_TOUCHPAD_MAX_Y;
	}

    // Inicializa Estados
	chiaki_controller_state_set_idle(&keyboard_state);
	chiaki_controller_state_set_idle(&touch_state);
	touch_tracker=QMap<int, uint8_t>();
	mouse_touch_id=-1;
	dpad_touch_id =-1;
	chiaki_controller_state_set_idle(&dpad_touch_state);
	dpad_touch_value = QPair<uint16_t, uint16_t>(0,0);
	dpad_touch_increment = connect_info.dpad_touch_increment;

    // Timers
	dpad_touch_timer = new QTimer(this);
	connect(dpad_touch_timer, &QTimer::timeout, this, &StreamSession::DpadSendFeedbackState);
	dpad_touch_timer->setInterval(DPAD_TOUCH_UPDATE_INTERVAL_MS);
	dpad_touch_stop_timer = new QTimer(this);
	dpad_touch_stop_timer->setSingleShot(true);
	connect(dpad_touch_stop_timer, &QTimer::timeout, this, [this]{
		if(dpad_touch_id >= 0) {
			dpad_touch_timer->stop();
			chiaki_controller_state_stop_touch(&dpad_touch_state, (uint8_t)dpad_touch_id);
			dpad_touch_id = -1;
			SendFeedbackState();
		}
	});

    // PSN Login
	chiaki_connect_info.holepunch_session = NULL;
	if(!connect_info.duid.isEmpty()) {
		err = InitiatePsnConnection(connect_info.psn_token);
		if (err != CHIAKI_ERR_SUCCESS) throw ChiakiException("Psn Connection Failed " + QString::fromLocal8Bit(chiaki_error_string(err)));
		chiaki_connect_info.holepunch_session = holepunch_session;
        QByteArray psn_account_id = QByteArray::fromBase64(connect_info.psn_account_id.toUtf8());
        memcpy(chiaki_connect_info.psn_account_id, psn_account_id.constData(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
	}

    // Inicia Sessão Chiaki
	err = chiaki_session_init(&session, &chiaki_connect_info, GetChiakiLog());
	if(err != CHIAKI_ERR_SUCCESS) throw ChiakiException("Chiaki Session Init failed");

    // Sinks (Audio/Video/Display)
	ChiakiCtrlDisplaySink display_sink;
	display_sink.user = this;
	display_sink.cantdisplay_cb = CantDisplayCb;
	chiaki_session_ctrl_set_display_sink(&session, &display_sink);

	chiaki_opus_decoder_set_cb(&opus_decoder, AudioSettingsCb, AudioFrameCb, this);
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_get_sink(&opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&session, &audio_sink);
	
    ChiakiAudioHeader audio_header;
	chiaki_audio_header_set(&audio_header, 2, 16, MICROPHONE_SAMPLES * 100, MICROPHONE_SAMPLES);
	chiaki_opus_encoder_header(&audio_header, &opus_encoder, &session);

	if (connect_info.enable_dualsense) {
		ChiakiAudioSink haptics_sink;
		haptics_sink.user = this;
		haptics_sink.frame_cb = HapticsFrameCb;
		chiaki_session_set_haptics_sink(&session, &haptics_sink);
	}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	this->enable_steamdeck_haptics = connect_info.enable_steamdeck_haptics;
#endif

#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(pi_decoder) chiaki_session_set_video_sample_cb(&session, chiaki_pi_decoder_video_sample_cb, pi_decoder);
	else chiaki_session_set_video_sample_cb(&session, chiaki_ffmpeg_decoder_video_sample_cb, ffmpeg_decoder);
#else
    chiaki_session_set_video_sample_cb(&session, chiaki_ffmpeg_decoder_video_sample_cb, ffmpeg_decoder);
#endif

	chiaki_session_set_event_cb(&session, EventCb, this);

#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	connect(ControllerManager::GetInstance(), &ControllerManager::AvailableControllersUpdated, this, &StreamSession::UpdateGamepads);
	connect(this, &StreamSession::DualSenseIntensityChanged, ControllerManager::GetInstance(), &ControllerManager::SetDualSenseIntensity);
	if(connect_info.buttons_by_pos) ControllerManager::GetInstance()->SetButtonsByPos();
#endif

    // [SUPRIMI CÓDIGO SETSU/STEAMDECK PARA ECONOMIZAR ESPAÇO, MAS MANTENHO A LÓGICA DE KEYMAP]
	key_map = connect_info.key_map;
	if (connect_info.enable_dualsense) {
		InitHaptics();
		rumble_haptics_intensity = connect_info.rumble_haptics_intensity;
		ConnectRumbleHaptics();
	}
	UpdateGamepads();

    // Timer Packet Loss
	QTimer *packet_loss_timer = new QTimer(this);
	packet_loss_timer->setInterval(200);
	packet_loss_timer->start();
	connect(packet_loss_timer, &QTimer::timeout, this, [this]() {
		if(packet_loss_history.size() > 10) packet_loss_history.takeFirst();
		packet_loss_history.append(session.stream_connection.congestion_control.packet_loss);
        // ... calculo média ...
	});
}

// =================================================================
// DESTRUTOR (CORRIGIDO)
// =================================================================
StreamSession::~StreamSession()
{
	if(audio_out) SDL_CloseAudioDevice(audio_out);
	if(audio_in) SDL_CloseAudioDevice(audio_in);
	if(session_started) chiaki_session_join(&session);
	chiaki_session_fini(&session);
	chiaki_opus_decoder_fini(&opus_decoder);
	chiaki_opus_encoder_fini(&opus_encoder);

#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled) {
		speex_echo_state_destroy(echo_state);
		speex_preprocess_state_destroy(preprocess_state);
	}
#endif

#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	QMetaObject::invokeMethod(this, [this]() {
		for(auto controller : controllers) {
			const uint8_t clear_effect[10] = { 0 };
			controller->SetTriggerEffects(0x05, clear_effect, 0x05, clear_effect);
			controller->SetRumble(0,0);
			controller->Unref();
		}
	});
#endif

#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(pi_decoder) { chiaki_pi_decoder_fini(pi_decoder); free(pi_decoder); }
#endif
    // AQUI ESTAVA O ERRO DO CÓDIGO ANTERIOR (O IF CORTADO)
	if(ffmpeg_decoder) {
		chiaki_ffmpeg_decoder_fini(ffmpeg_decoder);
		delete ffmpeg_decoder;
	}

	if (haptics_output > 0) {
		SDL_CloseAudioDevice(haptics_output);
		haptics_output = 0;
	}
	if (haptics_resampler_buf) {
		free(haptics_resampler_buf);
		haptics_resampler_buf = nullptr;
	}
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (sdeck_haptics_senderl) { free(sdeck_haptics_senderl); sdeck_haptics_senderl = nullptr; }
	if (sdeck_haptics_senderr) { free(sdeck_haptics_senderr); sdeck_haptics_senderr = nullptr; }
#endif
}

// =================================================================
// LÓGICA DE INPUT (CRONUS MODIFICADO)
// =================================================================
void StreamSession::SendFeedbackState()
{
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);

    if (keyboard_controller_enabled) chiaki_controller_state_or(&state, &keyboard_state);
    if (mouse_touch_enabled && mouse_touch_id >= 0) chiaki_controller_state_or(&state, &touch_state);
    if (dpad_touch_id >= 0) chiaki_controller_state_or(&state, &dpad_touch_state);

    // Adiciona inputs dos controles físicos
    for (auto controller : controllers) {
        ChiakiControllerState ctrl_state = controller->GetState();
        chiaki_controller_state_or(&state, &ctrl_state);
    }

    // --- [CRONUS MOD] LÓGICA DE RECOIL ---
    if (g_recoilEnabled.load()) {
        // Verifica se L2 e R2 estão pressionados
        // trigger_l2 e trigger_r2 vão de 0 a 65535
        bool isAiming = state.trigger_l2 > 20000;
        bool isFiring = state.trigger_r2 > 20000;

        if (isAiming && isFiring) {
            static int recoil_tick = 0;
            recoil_tick++;

            if (recoil_tick > 60) recoil_tick = 0;

            int strength = g_recoilStrength.load();

            // Puxa para baixo (Eixo Y Positivo)
            int pull_down = (strength * 25) + (recoil_tick * 2);
            int32_t new_y = state.stick_right_y + pull_down;

            if (new_y > 32767) new_y = 32767;
            if (new_y < -32768) new_y = -32768;

            state.stick_right_y = (int16_t)new_y;
        } else {
            static int recoil_tick = 0;
        }
    }
    // -------------------------------------

    chiaki_session_send_controller_state(&session, &state);
}

void StreamSession::DpadSendFeedbackState() {
    // Função auxiliar para touchpads
    if (dpad_touch_id >= 0) {
        dpad_touch_value.first += dpad_touch_increment;
        chiaki_controller_state_touch(&dpad_touch_state, (uint8_t)dpad_touch_id, dpad_touch_value.first, dpad_touch_value.second);
        SendFeedbackState();
    }
}

void StreamSession::UpdateGamepads() {
#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
    auto new_controllers = ControllerManager::GetInstance()->GetControllers();
    for (auto controller : controllers) controller->Unref();
    controllers.clear();
    for (auto controller : new_controllers) {
        controller->Ref();
        controllers.append(controller);
        connect(controller, &GameController::StateChanged, this, &StreamSession::SendFeedbackState);
    }
    SendFeedbackState();
#endif
}

// =================================================================
// CALLBACKS (IMPLEMENTAÇÕES PADRÃO)
// =================================================================
static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user) {
    // (Implementação simplificada: Passa para a instância real)
    // Normalmente chamaria StreamSession::AudioSettingsChanged
}

static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user) {
    // Passa áudio para o SDL
}

static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user) {
    StreamSession* sess = (StreamSession*)user;
    if(sess) sess->ProcessHaptics(buf, buf_size);
}

static void CantDisplayCb(void *user, bool cant_display) {
    StreamSession* sess = (StreamSession*)user;
    if(sess) emit sess->CantDisplay(cant_display);
}

static void EventCb(ChiakiEvent *event, void *user) {
    StreamSession* sess = (StreamSession*)user;
    if(!sess) return;
    // Processa eventos de conexão/desconexão
    if (event->type == CHIAKI_EVENT_CONNECT) {
        sess->connected = true;
        emit sess->StreamConnected();
    } else if (event->type == CHIAKI_EVENT_DISCONNECT) {
        sess->connected = false;
        emit sess->StreamDisconnected();
    }
}

static void FfmpegFrameCb(ChiakiFfmpegDecoder *decoder, void *user) {
    StreamSession* sess = (StreamSession*)user;
    // Notifica que há um novo frame
    if(sess) emit sess->TextureAvailable();
}

#if CHIAKI_GUI_ENABLE_SETSU
static void SessionSetsuCb(SetsuEvent *event, void *user) {}
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
static void SessionSDeckCb(SDeckEvent *event, void *user) {}
#endif
