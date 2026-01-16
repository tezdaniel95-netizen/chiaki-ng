#include "qmlmainwindow.h"
#include "qmlbackend.h"
#include "qmlsvgprovider.h"
#include "chiaki/log.h"
#include "streamsession.h"

#include <qpa/qplatformnativeinterface.h>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <QDebug>
#include <QThread>
#include <QShortcut>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QVulkanInstance>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickRenderTarget>
#include <QQuickRenderControl>
#include <QQuickGraphicsDevice>

// --- [INCLUDES NECESSÁRIOS PARA O MOUSE E CRONUS] ---
#include <QMouseEvent>
#include <QCursor>
#include <QDialog>
#include <QVBoxLayout>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <atomic>

#if defined(Q_OS_MACOS)
#include <objc/message.h>
#endif

// =================================================================
// [CRONUS MOD] CONEXÃO COM AS VARIÁVEIS GLOBAIS
// Dizemos ao arquivo que essas variáveis existem no streamsession.cpp
// =================================================================
extern std::atomic<bool> g_recoilEnabled;
extern std::atomic<int> g_recoilStrength;

// =================================================================
// CONSTRUTOR DA JANELA (ONDE CRIAMOS O MENU VISUAL)
// =================================================================
// Nota: Se der erro na linha abaixo, verifique se no seu qmlmainwindow.h 
// o construtor pede (QmlBackend *backend, QWindow *parent) ou apenas (QObject *parent).
// Ajustei para o padrão mais comum do Chiaki com QML.
QmlMainWindow::QmlMainWindow(QmlBackend *backend, QWindow *parent)
    : QQuickWindow(parent)
    , m_backend(backend)
{
    // Configurações iniciais padrão do Chiaki (se houver)
    
    // =============================================================
    // [CRONUS MOD] INTERFACE GRÁFICA FLUTUANTE
    // =============================================================
    
    // 1. Cria a janela independente (nullptr = sem pai fixo)
    QDialog* cronusMenu = new QDialog(nullptr);
    cronusMenu->setWindowTitle("Cronus Aim Assist");
    // Mantém a janela sempre visível por cima do jogo
    cronusMenu->setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    cronusMenu->resize(320, 160);
    cronusMenu->setStyleSheet("background-color: #121212; color: #00FF00;");

    QVBoxLayout* layout = new QVBoxLayout(cronusMenu);

    // Título
    QLabel* title = new QLabel("--- RECOIL CONTROL ---", cronusMenu);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-weight: bold; font-size: 14px; margin-bottom: 5px;");
    layout->addWidget(title);

    // Checkbox (Liga/Desliga)
    QCheckBox* chkEnable = new QCheckBox("ATIVAR SCRIPT", cronusMenu);
    chkEnable->setStyleSheet("font-size: 13px; font-weight: bold; padding: 5px;");
    // Conecta o clique à variável global
    QObject::connect(chkEnable, &QCheckBox::toggled, [](bool checked){
        g_recoilEnabled = checked;
    });
    layout->addWidget(chkEnable);

    // Slider (Força)
    QLabel* lblForce = new QLabel("Força Vertical (0):", cronusMenu);
    layout->addWidget(lblForce);

    QSlider* sldForce = new QSlider(Qt::Horizontal, cronusMenu);
    sldForce->setRange(0, 100);
    sldForce->setValue(0);
    
    // Atualiza a variável global e o texto quando mexe no slider
    QObject::connect(sldForce, &QSlider::valueChanged, [lblForce](int val){
        g_recoilStrength = val;
        lblForce->setText("Força Vertical (" + QString::number(val) + "):");
    });
    layout->addWidget(sldForce);

    // Mostra a janela imediatamente ao iniciar o Chiaki
    cronusMenu->show();
    
    // =============================================================
}

// Destrutor (Padrão)
QmlMainWindow::~QmlMainWindow()
{
    // Limpeza padrão se necessário
}

// =================================================================
// CÓDIGO DE MIRA (MOUSE AIM) & EVENTOS GERAIS
// =================================================================
bool QmlMainWindow::event(QEvent *ev)
{
    // Verifica se temos uma sessão ativa para processar o mouse
    // 'session' geralmente é acessado via backend->getSession() ou variável membro
    // Assumindo que 'session' está disponível como no seu código original
    
    bool isMouseMove = (ev->type() == QEvent::MouseMove);
    
    // Verifica se deve capturar o mouse (tem video, input grab ativo, session existe)
    if (isMouseMove && session && has_video && grab_input) {
        QMouseEvent *me = static_cast<QMouseEvent*>(ev);
        
        // --- CONFIGURAÇÕES DE MIRA ---
        int sensitivity = 450; 
        int deadzone = 2200;   
        
        // Pega o centro da janela
        int centerX = width() / 2;
        int centerY = height() / 2;
        
        // Calcula delta usando position() (Qt 6) ou localPos() (Qt 5)
        // Usando position().x() conforme seu código
        int deltaX = me->position().x() - centerX;
        int deltaY = me->position().y() - centerY;

        if (deltaX == 0 && deltaY == 0) return true;

        // Aplica Sensibilidade
        int finalX = deltaX * sensitivity;
        int finalY = deltaY * sensitivity;

        // Lógica Anti-Deadzone (Pula a zona morta do controle)
        if (finalX > 0) finalX += deadzone;
        if (finalX < 0) finalX -= deadzone;
        if (finalY > 0) finalY += deadzone;
        if (finalY < -2200) finalY -= deadzone; // Ajuste específico que você fez

        // Clamping (Limita aos valores máximos do DualShock/DualSense)
        if (finalX > 32767) finalX = 32767;
        if (finalX < -32767) finalX = -32767;
        if (finalY > 32767) finalY = 32767;
        if (finalY < -32767) finalY = -32767;

        // --- ENVIO DO INPUT ---
        // Aqui enviamos para a struct de Aim do Chiaki
        session->GetAimState().rightStickX = finalX;
        session->GetAimState().rightStickY = finalY;
        
        // Força o envio imediato do pacote
        session->ForceSendInput();

        // Recentraliza o mouse (Trava o mouse no centro da tela)
        QCursor::setPos(mapToGlobal(QPoint(centerX, centerY)));
        
        return true; 
    }

    // Passa outros eventos para o tratamento padrão do QWindow
    return QWindow::event(ev);
}
