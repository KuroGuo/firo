#include "test/test_bitcoin.h"
#include "test/fixtures.h"

#include "script/interpreter.h"
#include "script/standard.h"
#include "script/sign.h"
#include "utilmoneystr.h"
#include "base58.h"
#include "netbase.h"
#include "validation.h"
#include "messagesigner.h"
#include "keystore.h"

#include "evo/specialtx.h"
#include "evo/spork.h"

#include <boost/test/unit_test.hpp>

typedef std::map<COutPoint, std::pair<int, CAmount>> SimpleUTXOMap;

static SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransaction>& txs)
{
    SimpleUTXOMap utxos;
    CAmount balance = 0;
    for (size_t i = 0; i < txs.size(); i++) {
        auto& tx = txs[i];
        size_t const znode_output = tx.vout.size() > 6 ? FindZnodeOutput(tx) : 0;
        for (size_t j = 0; j < tx.vout.size(); j++) {
            if(j == 0 || j == znode_output) {
                balance += tx.vout[j].nValue;
                utxos.emplace(COutPoint(tx.GetHash(), j), std::make_pair((int)i + 1, tx.vout[j].nValue));
            }
        }
    }
    return utxos;
}

static std::vector<COutPoint> SelectUTXOs(SimpleUTXOMap& utoxs, CAmount amount, CAmount& changeRet)
{
    changeRet = 0;

    std::vector<COutPoint> selectedUtxos;
    CAmount selectedAmount = 0;
    while (!utoxs.empty()) {
        bool found = false;
        for (auto it = utoxs.begin(); it != utoxs.end(); ++it) {
            if (chainActive.Height() - it->second.first < 101) {
                continue;
            }

            found = true;
            selectedAmount += it->second.second;
            selectedUtxos.emplace_back(it->first);
            utoxs.erase(it);
            break;
        }
        BOOST_ASSERT(found);
        if (selectedAmount >= amount) {
            changeRet = selectedAmount - amount;
            break;
        }
    }

    return selectedUtxos;
}

static void FundTransaction(CMutableTransaction& tx, SimpleUTXOMap& utoxs, const CScript& scriptPayout, CAmount amount, const CKey& coinbaseKey)
{
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    CAmount change;
    auto inputs = SelectUTXOs(utoxs, amount, change);
    for (size_t i = 0; i < inputs.size(); i++) {
        tx.vin.emplace_back(CTxIn(inputs[i]));
    }
    tx.vout.emplace_back(CTxOut(amount, scriptPayout));
    if (change != 0) {
        tx.vout.emplace_back(CTxOut(change, scriptPayout));
    }
}

static void SignTransaction(CMutableTransaction& tx, const CKey& coinbaseKey)
{
    CBasicKeyStore tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        CTransactionRef txFrom;
        uint256 hashBlock;
        BOOST_ASSERT(GetTransaction(tx.vin[i].prevout.hash, txFrom, Params().GetConsensus(), hashBlock));
        bool result = SignSignature(tempKeystore, *txFrom, tx, i, SIGHASH_ALL);
        if(!result)
            std::cerr << i << std::endl;
    }
}

static CMutableTransaction CreateSporkTx(SimpleUTXOMap &utxos, const CKey &coinbaseKey, const std::vector<CSporkAction> &actions)
{
    CBitcoinSecret secretKey;
    secretKey.SetString("cW2YM2xaeCaebfpKguBahUAgEzLXgSserWRuD29kSyKHq1TTgwRQ");

    CSporkTx sporkTxPayload;
    sporkTxPayload.actions = actions;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_SPORK;

    CScript scriptPayout;
    FundTransaction(tx, utxos, scriptPayout, COIN/10, coinbaseKey);
    sporkTxPayload.inputsHash = CalcTxInputsHash(tx);
    CHashSigner::SignHash(::SerializeHash(sporkTxPayload), secretKey.GetKey(), sporkTxPayload.vchSig);
    SetTxPayload(tx, sporkTxPayload);
    SignTransaction(tx, coinbaseKey);

    return tx;
}

BOOST_FIXTURE_TEST_SUITE(evospork_tests, LelantusTestingSetup)

BOOST_AUTO_TEST_CASE(evospork_general)
{
    GenerateBlocks(900);

    auto utxos = BuildSimpleUtxoMap(coinbaseTxns);
    CMutableTransaction tx = CreateSporkTx(utxos, coinbaseKey, {
        {CSporkAction::sporkDisable, CSporkAction::featureLelantus, 0, 1100}});

    CreateAndProcessBlock({tx}, coinbaseKey);

    std::vector<CMutableTransaction> lelantusMints;
    GenerateMints({1,2}, lelantusMints);

    int height = chainActive.Height();
    CreateAndProcessBlock(lelantusMints, coinbaseKey);
    BOOST_ASSERT(chainActive.Height() == height);
}

BOOST_AUTO_TEST_SUITE_END()
