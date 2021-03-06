/*
 * QEstEidUtil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "QSmartCard_p.h"

#include <common/IKValidator.h>
//#include <common/PinDialog.h>
#include "TokenData.h"
///// TODO ???? #include <common/Settings.h>

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QScopedPointer>
#include <QtNetwork/QSslKey>
#include <QtWidgets/QApplication>

#include <openssl/evp.h>
#include <thread>

#include "poc/logging.h"

QSmartCardData::QSmartCardData(): d(new QSmartCardDataPrivate) {}
QSmartCardData::QSmartCardData(const QSmartCardData &other): d(other.d) {}
QSmartCardData::~QSmartCardData() {}
QSmartCardData& QSmartCardData::operator =(const QSmartCardData &other) { d = other.d; return *this; }

QString QSmartCardData::card() const { return d->card; }
QStringList QSmartCardData::cards() const { return d->cards; }

bool QSmartCardData::isNull() const
{ return d->data.isEmpty() && d->authCert.isNull() && d->signCert.isNull(); }
bool QSmartCardData::isPinpad() const { return d->pinpad; }
bool QSmartCardData::isSecurePinpad() const
{ return d->reader.contains("EZIO SHIELD", Qt::CaseInsensitive); }
bool QSmartCardData::isValid() const
{ return d->data.value(Expiry).toDateTime() >= QDateTime::currentDateTime(); }

QString QSmartCardData::reader() const { return d->reader; }
QStringList QSmartCardData::readers() const { return d->readers; }

QVariant QSmartCardData::data(PersonalDataType type) const
{ return d->data.value(type); }
SslCertificate QSmartCardData::authCert() const { return d->authCert; }
SslCertificate QSmartCardData::signCert() const { return d->signCert; }
quint8 QSmartCardData::retryCount(PinType type) const { return d->retry.value(type); }
ulong QSmartCardData::usageCount(PinType type) const { return d->usage.value(type); }
QSmartCardData::CardVersion QSmartCardData::version() const { return d->version; }

QString QSmartCardData::typeString(QSmartCardData::PinType type)
{
	switch(type)
	{
	case Pin1Type: return "PIN1";
	case Pin2Type: return "PIN2";
	case PukType: return "PUK";
	}
	return "";
}



QSharedPointer<QPCSCReader> QSmartCardPrivate::connect(const QString &reader)
{
	TERA_LOG(debug) << "Connecting to reader" << reader;
	QSharedPointer<QPCSCReader> r(new QPCSCReader(reader, &QPCSC::instance()));
	if(r->connect() && r->beginTransaction())
		return r;
	return QSharedPointer<QPCSCReader>();
}

QSmartCard::ErrorType QSmartCardPrivate::handlePinResult(QPCSCReader *reader, QPCSCReader::Result response, bool forceUpdate)
{
	if(!response.resultOk() || forceUpdate)
		updateCounters(reader, t.d.data());
	switch((quint8(response.SW[0]) << 8) + quint8(response.SW[1]))
	{
	case 0x9000: return QSmartCard::NoError;
	case 0x63C0: return QSmartCard::BlockedError;//pin retry count 0
	case 0x63C1: // Validate error, 1 tries left
	case 0x63C2: // Validate error, 2 tries left
	case 0x63C3: return QSmartCard::ValidateError;
	case 0x6400: // Timeout (SCM)
	case 0x6401: return QSmartCard::CancelError; // Cancel (OK, SCM)
	case 0x6402: return QSmartCard::DifferentError;
	case 0x6403: return QSmartCard::LenghtError;
	case 0x6983: return QSmartCard::BlockedError;
	case 0x6985: return QSmartCard::OldNewPinSameError;
	case 0x6A80: return QSmartCard::OldNewPinSameError;
	default: return QSmartCard::UnknownError;
	}
}

quint16 QSmartCardPrivate::language() const
{
/////////////////////////////////////////////////////// TODO XXXXXXXXXXXXXXXXXXXXXX
	//if(Settings().language() == "en") return 0x0409;
	//if(Settings().language() == "et") return 0x0425;
	//if(Settings().language() == "ru") return 0x0419;
	return 0x0000;
}

QHash<quint8,QByteArray> QSmartCardPrivate::parseFCI(const QByteArray &data) const
{
	QHash<quint8,QByteArray> result;
	for(QByteArray::const_iterator i = data.constBegin(); i != data.constEnd(); ++i)
	{
		quint8 tag(*i), size(*++i);
		result[tag] = QByteArray(i + 1, size);
		switch(tag)
		{
		case 0x6F:
		case 0x62:
		case 0x64:
		case 0xA1: continue;
		default: i += size; break;
		}
	}
	return result;
}

int QSmartCardPrivate::rsa_sign(int type, const unsigned char *m, unsigned int m_len,
		unsigned char *sigret, unsigned int *siglen, const RSA *rsa)
{
	QSmartCardPrivate *d = (QSmartCardPrivate*)RSA_get_app_data(rsa);
	if(type != NID_md5_sha1 ||
		m_len != 36 ||
		!d ||
		!d->reader ||
		!d->reader->transfer(d->SECENV1).resultOk() ||
		!d->reader->transfer(APDU("002241B8 02 8300")).resultOk()) //Key reference, 8303801100
		return 0;

	QByteArray cmd = APDU("0088000000"); //calc signature
	cmd[4] = m_len;
	cmd += QByteArray::fromRawData((const char*)m, m_len);
	QPCSCReader::Result result = d->reader->transfer(cmd);
	if(!result.resultOk()) {
		return 0;
    }

	*siglen = (unsigned int)result.data.size();
	memcpy(sigret, result.data.constData(), result.data.size());
	return 1;
}

bool QSmartCardPrivate::updateCounters(QPCSCReader *reader, QSmartCardDataPrivate *d)
{
	if(!reader->transfer(MASTER_FILE).resultOk() ||
		!reader->transfer(PINRETRY).resultOk())
		return false;

	QByteArray cmd = READRECORD;
	for(int i = QSmartCardData::Pin1Type; i <= QSmartCardData::PukType; ++i)
	{
		cmd[2] = i;
		QPCSCReader::Result data = reader->transfer(cmd);
		if(!data.resultOk())
			return false;
		d->retry[QSmartCardData::PinType(i)] = data.data[5];
	}

	if(!reader->transfer(ESTEIDDF).resultOk() ||
		!reader->transfer(KEYPOINTER).resultOk())
		return false;

	cmd[2] = 1;
	QPCSCReader::Result data = reader->transfer(cmd);
	if(!data.resultOk())
		return false;

	/*
	 * SIGN1 0100 1
	 * SIGN2 0200 2
	 * AUTH1 1100 3
	 * AUTH2 1200 4
	 */
	quint8 signkey = data.data.at(0x13) == 0x01 && data.data.at(0x14) == 0x00 ? 1 : 2;
	quint8 authkey = data.data.at(0x09) == 0x11 && data.data.at(0x0A) == 0x00 ? 3 : 4;

	if(!reader->transfer(KEYUSAGE).resultOk())
		return false;

	cmd[2] = authkey;
	data = reader->transfer(cmd);
	if(!data.resultOk())
		return false;
	d->usage[QSmartCardData::Pin1Type] = 0xFFFFFF - ((quint8(data.data[12]) << 16) + (quint8(data.data[13]) << 8) + quint8(data.data[14]));

	cmd[2] = signkey;
	data = reader->transfer(cmd);
	if(!data.resultOk())
		return false;
	d->usage[QSmartCardData::Pin2Type] = 0xFFFFFF - ((quint8(data.data[12]) << 16) + (quint8(data.data[13]) << 8) + quint8(data.data[14]));
	return true;
}




QSmartCard::QSmartCard(PinDialogFactory& p, QObject *parent)
:	QThread(parent)
,	d(new QSmartCardPrivate)
, pdf(p)
{
#ifdef TERA_OLD_OPENSSL
    d->method->name = "QSmartCard";
    d->method->rsa_sign = QSmartCardPrivate::rsa_sign;
#else
    RSA_meth_set1_name(d->method, "QSmartCard");
    RSA_meth_set_sign(d->method, QSmartCardPrivate::rsa_sign);
#endif
	d->t.d->readers = QPCSC::instance().readers();
	d->t.d->cards = QStringList() << "loading";
	d->t.d->card = "loading";
}

QSmartCard::~QSmartCard()
{
	d->terminate = true;
	wait();
	delete d;
}

QSmartCard::ErrorType QSmartCard::change(QSmartCardData::PinType type, const QString &newpin, const QString &pin)
{
	QMutexLocker locker(&d->m);
	QSharedPointer<QPCSCReader> reader(d->connect(d->t.reader()));
	if(!reader)
		return UnknownError;
	QByteArray cmd = d->CHANGE;
	cmd[3] = type == QSmartCardData::PukType ? 0 : type;
	cmd[4] = pin.size() + newpin.size();
	QPCSCReader::Result result;
	if(d->t.isPinpad())
	{
		QEventLoop l;
		std::thread([&]{
			result = reader->transferCTL(cmd, false, d->language(), [](QSmartCardData::PinType type){
				switch(type)
				{
				default:
				case QSmartCardData::Pin1Type: return 4;
				case QSmartCardData::Pin2Type: return 5;
				case QSmartCardData::PukType: return 8;
				}
			}(type));
			l.quit();
		}).detach();
		l.exec();
	}
	else
		result = reader->transfer(cmd + pin.toUtf8() + newpin.toUtf8());
	return d->handlePinResult(reader.data(), result, true);
}

QSmartCardData QSmartCard::data() const {return d->t; }

QSmartCardData QSmartCard::dataXXX() {
    QMutexLocker locker(&bufferedDataMutex);
    return bufferedData; // return d->t;
}



QSslKey QSmartCard::key()
{
	RSA *rsa = RSAPublicKey_dup((RSA*)d->t.authCert().publicKey().handle());
	if (!rsa)
		return QSslKey();

	RSA_set_method(rsa, d->method);
#ifdef TERA_OLD_OPENSSL
	rsa->flags |= RSA_FLAG_SIGN_VER;
#endif
	RSA_set_app_data(rsa, d);
	EVP_PKEY *key = EVP_PKEY_new();
	EVP_PKEY_set1_RSA(key, rsa);
	RSA_free(rsa);
	return QSslKey(key);
}

QSmartCard::ErrorType QSmartCard::login(QSmartCardData::PinType type)
{
    PinDialogInterface::PinFlags flags = PinDialogInterface::Pin1Type;
	QSslCertificate cert;
	switch(type)
	{
    case QSmartCardData::Pin1Type: flags = PinDialogInterface::Pin1Type; cert = d->t.authCert(); break;
    case QSmartCardData::Pin2Type: flags = PinDialogInterface::Pin2Type; cert = d->t.signCert(); break;
	default: return UnknownError;
	}

	QScopedPointer<PinDialogInterface> p;
	QByteArray pin;
	if(!d->t.isPinpad())
	{
		p.reset(pdf.createPinDialog(flags, cert));
		if(!p->execDialog())
			return CancelError;
		pin = p->getPin();
	}
	else
        p.reset(pdf.createPinDialog(PinDialogInterface::PinFlags(flags | PinDialogInterface::PinpadFlag), cert));

	d->m.lock();
	d->reader = d->connect(d->t.reader());
	if(!d->reader) {
        //d->updateCounters(d->reader.data(), d->t.d);
        d->reader.clear();
        d->m.unlock();
		return UnknownError;
    }
	QByteArray cmd = d->VERIFY;
    cmd[3] = type;
	cmd[4] = pin.size();
	QPCSCReader::Result result;
	if(d->t.isPinpad())
	{
		std::thread([&]{
            p->doStartTimer();
			result = d->reader->transferCTL(cmd, true, d->language());
            p->doFinish(0);
		}).detach();
		p->execDialog();
	}
	else
		result = d->reader->transfer(cmd + pin);
	QSmartCard::ErrorType err = d->handlePinResult(d->reader.data(), result, false);
	if(!result.resultOk())
	{
		d->updateCounters(d->reader.data(), d->t.d.data());
		d->reader.clear();
		d->m.unlock();
	}
	return err;
}

void QSmartCard::logout()
{
	if(d->reader.isNull())
		return;
	d->updateCounters(d->reader.data(), d->t.d.data());
	d->reader.clear();
	d->m.unlock();
}

void QSmartCard::reload() { selectCard(d->t.card());  }

void QSmartCard::emitDataChanged() {
    {
        QMutexLocker locker(&bufferedDataMutex);
        bufferedData = d->t;
    }
    Q_EMIT dataChanged();
}

void QSmartCard::run()
{
	static const QHash<QByteArray,QSmartCardData::CardVersion> atrList{
		{"3BFE9400FF80B1FA451F034573744549442076657220312E3043", QSmartCardData::VER_1_0}, /*ESTEID_V1_COLD_ATR*/
		{"3B6E00FF4573744549442076657220312E30", QSmartCardData::VER_1_0}, /*ESTEID_V1_WARM_ATR*/
		{"3BDE18FFC080B1FE451F034573744549442076657220312E302B", QSmartCardData::VER_1_0_2007}, /*ESTEID_V1_2007_COLD_ATR*/
		{"3B5E11FF4573744549442076657220312E30", QSmartCardData::VER_1_0_2007}, /*ESTEID_V1_2007_WARM_ATR*/
		{"3B6E00004573744549442076657220312E30", QSmartCardData::VER_1_1}, /*ESTEID_V1_1_COLD_ATR*/
		{"3BFE1800008031FE454573744549442076657220312E30A8", QSmartCardData::VER_3_4}, /*ESTEID_V3_COLD_DEV1_ATR*/
		{"3BFE1800008031FE45803180664090A4561B168301900086", QSmartCardData::VER_3_4}, /*ESTEID_V3_WARM_DEV1_ATR*/
		{"3BFE1800008031FE45803180664090A4162A0083019000E1", QSmartCardData::VER_3_4}, /*ESTEID_V3_WARM_DEV2_ATR*/
		{"3BFE1800008031FE45803180664090A4162A00830F9000EF", QSmartCardData::VER_3_4}, /*ESTEID_V3_WARM_DEV3_ATR*/
		{"3BF9180000C00A31FE4553462D3443432D303181", QSmartCardData::VER_3_5}, /*ESTEID_V35_COLD_DEV1_ATR*/
		{"3BF81300008131FE454A434F5076323431B7", QSmartCardData::VER_3_5}, /*ESTEID_V35_COLD_DEV2_ATR*/
		{"3BFA1800008031FE45FE654944202F20504B4903", QSmartCardData::VER_3_5}, /*ESTEID_V35_COLD_DEV3_ATR*/
		{"3BFE1800008031FE45803180664090A4162A00830F9000EF", QSmartCardData::VER_3_5}, /*ESTEID_V35_WARM_ATR*/
		{"3BFE1800008031FE45803180664090A5102E03830F9000EF", QSmartCardData::VER_3_5}, /*UPDATER_TEST_CARDS*/
	};

	QByteArray cardid = d->READRECORD;
	cardid[2] = 8;

	while(!d->terminate)
	{

		if(d->m.tryLock())
		{
			// Get list of available cards
			QMap<QString,QString> cards;
			const QStringList readers = QPCSC::instance().readers();
			if(![&] {
				for(const QString &name: readers)
				{
					TERA_LOG(debug) << "Connecting to reader" << name;
					QScopedPointer<QPCSCReader> reader(new QPCSCReader(name, &QPCSC::instance()));
					if(!reader->isPresent())
						continue;

					if(!atrList.contains(reader->atr()))
					{
						TERA_LOG(debug) << "Unknown ATR" << reader->atr();
						continue;
					}

					switch(reader->connectEx())
					{
					case 0x8010000CL: continue; //SCARD_E_NO_SMARTCARD
					case 0:
						if(reader->beginTransaction())
							break;
					default: return false;
					}

					QPCSCReader::Result result;
					#define TRANSFERIFNOT(X) result = reader->transfer(X); \
						if(result.err) return false; \
						if(!result.resultOk())

					TRANSFERIFNOT(d->MASTER_FILE)
					{	// Master file selection failed, test if it is updater applet
						TRANSFERIFNOT(d->UPDATER_AID)
							continue; // Updater applet not found
						TRANSFERIFNOT(d->MASTER_FILE)
						{	//Found updater applet but cannot select master file, select back 3.5
							reader->transfer(d->AID35);
							continue;
						}
					}
					TRANSFERIFNOT(d->ESTEIDDF)
						continue;
					TRANSFERIFNOT(d->PERSONALDATA)
						continue;
					TRANSFERIFNOT(cardid)
						continue;
					QString nr = d->codec->toUnicode(result.data);
					if(!nr.isEmpty())
						cards[nr] = name;
				}
				return true;
			}())
			{
				TERA_LOG(debug) << "Failed to poll card, try again next round";
				d->m.unlock();
				sleep(5);
				continue;
			}

			// cardlist has changed
			QStringList order = cards.keys();
			std::sort(order.begin(), order.end(), TokenData::cardsOrder);
			bool update = d->t.cards() != order || d->t.readers() != readers;

			// check if selected card is still in slot
			if(!d->t.card().isEmpty() && !order.contains(d->t.card()))
			{
				update = true;
				d->t.d = new QSmartCardDataPrivate();
			}

			d->t.d->cards = order;
			d->t.d->readers = readers;

			// if none is selected select first from cardlist
			if(d->t.card().isEmpty() && !d->t.cards().isEmpty())
			{
				QExplicitlySharedDataPointer<QSmartCardDataPrivate> t = d->t.d;
				t.detach();
				t->card = d->t.cards().first();
				t->data.clear();
				t->authCert = QSslCertificate();
				t->signCert = QSslCertificate();
				d->t.d = t;
				update = true;
                emitDataChanged();
			}

			// read card data
			if(d->t.cards().contains(d->t.card()) && d->t.isNull())
			{
				update = true;
				QSharedPointer<QPCSCReader> reader(d->connect(cards.value(d->t.card())));
				if(!reader.isNull())
				{
					QExplicitlySharedDataPointer<QSmartCardDataPrivate> t = d->t.d;
					t.detach();
					t->reader = reader->name();
					t->pinpad = reader->isPinPad();
					t->version = atrList.value(reader->atr(), QSmartCardData::VER_INVALID);
					if(t->version > QSmartCardData::VER_1_1)
					{
						if(reader->transfer(d->AID30).resultOk())
							t->version = QSmartCardData::VER_3_0;
						else if(reader->transfer(d->AID34).resultOk())
							t->version = QSmartCardData::VER_3_4;
						else if(reader->transfer(d->UPDATER_AID).resultOk())
						{
							t->version = QSmartCardData::CardVersion(t->version|QSmartCardData::VER_HASUPDATER);
							//Prefer EstEID applet when if it is usable
							if(!reader->transfer(d->AID35).resultOk() ||
								!reader->transfer(d->MASTER_FILE).resultOk())
							{
								reader->transfer(d->UPDATER_AID);
								t->version = QSmartCardData::VER_USABLEUPDATER;
							}
						}
					}

					bool tryAgain = !d->updateCounters(reader.data(), t.data());
					if(reader->transfer(d->PERSONALDATA).resultOk())
					{
						QByteArray cmd = d->READRECORD;
						for(int data = QSmartCardData::SurName; data != QSmartCardData::Comment4; ++data)
						{
							cmd[2] = data + 1;
							QPCSCReader::Result result = reader->transfer(cmd);
							if(!result.resultOk())
							{
								tryAgain = true;
								break;
							}
							QString record = d->codec->toUnicode(result.data.trimmed());
							if(record == QChar(0))
								record.clear();
							switch(data)
							{
							case QSmartCardData::BirthDate:
							case QSmartCardData::Expiry:
							case QSmartCardData::IssueDate:
								t->data[QSmartCardData::PersonalDataType(data)] = QDate::fromString(record, "dd.MM.yyyy");
								break;
							default:
								t->data[QSmartCardData::PersonalDataType(data)] = record;
								break;
							}
						}
					}

					auto readCert = [&](const QByteArray &file) {
						QPCSCReader::Result data = reader->transfer(file + APDU(reader->protocol() == QPCSCReader::T1 ? "00" : ""));
						if(!data.resultOk())
							return QSslCertificate();
						QHash<quint8,QByteArray> fci = d->parseFCI(data.data);
						int size = fci.contains(0x85) ? fci[0x85][0] << 8 | fci[0x85][1] : 0x0600;
						QByteArray cert;
						while(cert.size() < size)
						{
							QByteArray cmd = d->READBINARY;
							cmd[2] = cert.size() >> 8;
							cmd[3] = cert.size();
							data = reader->transfer(cmd);
							if(!data.resultOk())
							{
								tryAgain = true;
								return QSslCertificate();
							}
							cert += data.data;
						}
						return QSslCertificate(cert, QSsl::Der);
					};
					t->authCert = readCert(d->AUTHCERT);
					t->signCert = readCert(d->SIGNCERT);

					t->data[QSmartCardData::Email] = t->authCert.subjectAlternativeNames().values(QSsl::EmailEntry).value(0);
					if(t->authCert.type() & SslCertificate::DigiIDType)
					{
						t->data[QSmartCardData::SurName] = t->authCert.toString("SN");
						t->data[QSmartCardData::FirstName1] = t->authCert.toString("GN");
						t->data[QSmartCardData::FirstName2] = QString();
						t->data[QSmartCardData::Id] = t->authCert.subjectInfo("serialNumber");
						t->data[QSmartCardData::BirthDate] = IKValidator::birthDate(t->authCert.subjectInfo("serialNumber"));
						t->data[QSmartCardData::IssueDate] = t->authCert.effectiveDate();
						t->data[QSmartCardData::Expiry] = t->authCert.expiryDate();
					}
					if(tryAgain)
					{
						TERA_LOG(debug) << "Failed to read card info, try again next round";
						update = false;
					}
					else
						d->t.d = t;
				}
			}

			// update data if something has changed
			if(update)
                emitDataChanged();
			d->m.unlock();
		} else {
            if(d != nullptr && d->reader != nullptr && !d->reader.isNull()) {
                // keep-alive for SC
                d->reader->getstatus();
            }
        }
		sleep(2);
	}
}

void QSmartCard::selectCard(const QString &card)
{
	QMutexLocker locker(&d->m);
	QExplicitlySharedDataPointer<QSmartCardDataPrivate> t = d->t.d;
	t.detach();
	t->card = card;
	t->data.clear();
	t->authCert = QSslCertificate();
	t->signCert = QSslCertificate();
	d->t.d = t;
    emitDataChanged();
}

QSmartCard::ErrorType QSmartCard::unblock(QSmartCardData::PinType type, const QString &pin, const QString &puk)
{
	QMutexLocker locker(&d->m);
	QSharedPointer<QPCSCReader> reader(d->connect(d->t.reader()));
	if(!reader)
		return UnknownError;

	// Make sure pin is locked
	QByteArray cmd = d->VERIFY;
	cmd[3] = type;
	cmd[4] = pin.size() + 1;
	for(int i = 0; i <= d->t.retryCount(type); ++i)
		reader->transfer(cmd + QByteArray(pin.size(), '0') + QByteArray::number(i));

	//Verify PUK
	cmd[3] = 0;
	cmd[4] = puk.size();
	QPCSCReader::Result result;
	if(d->t.isPinpad())
	{
		QEventLoop l;
		std::thread([&]{
			result = reader->transferCTL(cmd, true, d->language(), 8);
			l.quit();
		}).detach();
		l.exec();
	}
	else
		result = reader->transfer(cmd + puk.toUtf8());
	if(!result.resultOk())
		return d->handlePinResult(reader.data(), result, false);

	//Replace PIN with PUK
	cmd = d->REPLACE;
	cmd[3] = type;
	cmd[4] = puk.size() + pin.size();
	if(d->t.isPinpad())
	{
		QEventLoop l;
		std::thread([&]{
			result = reader->transferCTL(cmd, false, d->language(), [](QSmartCardData::PinType type){
				switch(type)
				{
				default:
				case QSmartCardData::Pin1Type: return 4;
				case QSmartCardData::Pin2Type: return 5;
				case QSmartCardData::PukType: return 8;
				}
			}(type));
			l.quit();
		}).detach();
		l.exec();
	}
	else
		result = reader->transfer(cmd + puk.toUtf8() + pin.toUtf8());
	return d->handlePinResult(reader.data(), result, true);
}
