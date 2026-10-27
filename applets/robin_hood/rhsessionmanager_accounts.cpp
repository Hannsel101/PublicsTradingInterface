#include "rhsessionmanager.h"

#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

#include <limits>
#include <utility>

namespace {
QString money(const QJsonValue &value)
{
    if (!value.isString() && !value.isDouble())
        return {};
    static const QRegularExpression decimal(QStringLiteral(R"(^\s*(-?)(\d+)(?:\.(\d+))?\s*$)"));
    const auto match = decimal.match(value.toVariant().toString());
    if (!match.hasMatch())
        return {};
    bool valid = false;
    qint64 whole = match.captured(2).toLongLong(&valid);
    if (!valid || whole == std::numeric_limits<qint64>::max())
        return {};
    const QString digits = match.captured(3);
    int cents = digits.left(2).leftJustified(2, QLatin1Char('0')).toInt();
    if (digits.size() > 2 && digits.at(2) >= QLatin1Char('5'))
        ++cents;
    if (cents == 100) {
        ++whole;
        cents = 0;
    }
    const QString sign = match.captured(1) == QStringLiteral("-") && (whole || cents)
        ? QStringLiteral("-") : QString();
    return sign + QStringLiteral("$")
        + QLocale(QLocale::English, QLocale::UnitedStates).toString(whole)
        + QStringLiteral(".") + QStringLiteral("%1").arg(cents, 2, 10, QLatin1Char('0'));
}

QString balance(const QJsonObject &item)
{
    for (const char *key : {"total_equity", "equity", "portfolio_equity"}) {
        const QString amount = money(item.value(QLatin1String(key)));
        if (!amount.isEmpty())
            return amount;
    }
    // Cash is not total equity for a margin account holding securities.
    if (item.value(QStringLiteral("type")).toString().compare(QStringLiteral("margin"), Qt::CaseInsensitive) != 0) {
        const QJsonValue cash = item.value(QStringLiteral("cash_balances"));
        if (cash.isObject()) {
            for (const char *key : {"cash", "total_cash", "cash_balance"}) {
                const QString amount = money(cash.toObject().value(QLatin1String(key)));
                if (!amount.isEmpty())
                    return amount;
            }
        } else if (const QString amount = money(cash); !amount.isEmpty()) {
            return amount;
        }
    }
    return QStringLiteral("Unavailable");
}

QString displayName(const QJsonObject &item)
{
    // Brokerage product is distinct from cash/margin settlement mechanics.
    if (item.contains("brokerage_account_type")) {
        const QString product = item.value("brokerage_account_type").toString().trimmed().toLower();
        if (product == "individual") return QStringLiteral("Standard Brokerage");
        if (product == "ira_roth") return QStringLiteral("Roth IRA");
        if (product == "ira_traditional") return QStringLiteral("IRA");
        return QStringLiteral("Account type unavailable");
    }
    QStringList labels;
    for (const char *key : {"type", "rh_account_type", "account_type", "retirement_type"})
        if (item.value(QLatin1String(key)).isString())
            labels.append(item.value(QLatin1String(key)).toString());
    const auto retirement = item.value("retirement");
    if (retirement.isString()) labels.append(retirement.toString());
    if (retirement.isObject()) {
        const auto details = retirement.toObject();
        for (const char *key : {"type", "rh_account_type", "account_type", "subtype"})
            if (details.value(QLatin1String(key)).isString()) labels.append(details.value(QLatin1String(key)).toString());
    }
    bool standard = false, traditional = false, roth = false, unknown = false, retirementMarker = false;
    for (QString label : labels) {
        label = label.trimmed().toLower().replace(QRegularExpression("[ -]+"), "_");
        if (QStringList{"cash", "margin", "individual", "brokerage", "standard"}.contains(label)) standard = true;
        else if (QStringList{"traditional", "traditional_ira", "ira_traditional"}.contains(label)) traditional = true;
        else if (QStringList{"roth", "roth_ira", "ira_roth"}.contains(label)) roth = true;
        else if (label == "retirement" || label == "ira") retirementMarker = true;
        else if (!label.isEmpty()) unknown = true;
    }
    if (unknown || (traditional && roth)) return QStringLiteral("Account type unavailable");
    if (roth) return QStringLiteral("Roth IRA");
    if (traditional) return QStringLiteral("IRA");
    if (retirementMarker || retirement.isObject() || retirement.isString() || item.value("is_retirement").toBool())
        return QStringLiteral("Account type unavailable");
    return standard ? QStringLiteral("Standard Brokerage") : QStringLiteral("Account type unavailable");
}
}

int RHAccountModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant RHAccountModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Account &account = m_items.at(index.row());
    switch (role) {
    case DisplayNameRole: return account.displayName;
    case AccountNumberRole: return account.accountNumber;
    case CurrentBalanceRole: return account.currentBalance;
    case HoldingsRole: return account.holdings;
    case HoldingsUpdatedAtRole: return account.holdingsUpdatedAt;
    case HoldingsStatusRole: return account.holdingsStatus;
    case HoldingsErrorRole: return account.holdingsError;
    default: return {};
    }
}

QHash<int, QByteArray> RHAccountModel::roleNames() const
{
    return {{DisplayNameRole, "displayName"}, {AccountNumberRole, "accountNumber"},
            {CurrentBalanceRole, "currentBalance"}, {HoldingsRole, "holdings"},
            {HoldingsUpdatedAtRole, "holdingsUpdatedAt"}, {HoldingsStatusRole, "holdingsStatus"},
            {HoldingsErrorRole, "holdingsError"}};
}

void RHAccountModel::setAccounts(const QJsonArray &results)
{
    QList<Account> accounts;
    for (const QJsonValue &value : results) {
        if (!value.isObject())
            continue;
        const QJsonObject item = value.toObject();
        QString accountNumber = item.value(QStringLiteral("account_number")).toString();
        if (accountNumber.isEmpty())
            accountNumber = item.value(QStringLiteral("rhs_account_number")).toString();
        accounts.append({displayName(item), accountNumber, balance(item)});
    }
    beginResetModel();
    m_items = std::move(accounts);
    endResetModel();
}

void RHAccountModel::setCachedAccounts(const QVariantList &accounts)
{
    QList<Account> items;
    for (const auto &value : accounts) {
        const auto row = value.toMap();
        items.append({row.value("displayName").toString(), row.value("accountNumber").toString(),
            row.value("currentBalance").toString(), row.value("holdings").toList(),
            row.value("holdingsUpdatedAt").toString(), row.value("holdingsStatus", "unavailable").toString(),
            row.value("holdingsError").toString()});
    }
    beginResetModel(); m_items = std::move(items); endResetModel();
}

void RHAccountModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
}
