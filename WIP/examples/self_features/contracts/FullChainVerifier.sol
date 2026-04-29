// SPDX-License-Identifier: GPL-3.0
/*
    Copyright 2021 0KIMS association.

    This file is generated with [snarkJS](https://github.com/iden3/snarkjs).

    snarkJS is a free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    snarkJS is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
    License for more details.

    You should have received a copy of the GNU General Public License
    along with snarkJS. If not, see <https://www.gnu.org/licenses/>.
*/

pragma solidity >=0.7.0 <0.9.0;

contract FullChainVerifier {
    // Scalar field size
    uint256 constant r    = 21888242871839275222246405745257275088548364400416034343698204186575808495617;
    // Base field size
    uint256 constant q   = 21888242871839275222246405745257275088696311157297823662689037894645226208583;

    // Verification Key data
    uint256 constant alphax  = 6620987413051955058814993897141759586028201560323347074975286624373393270484;
    uint256 constant alphay  = 4425756094425641053896407889695030276102588683571611299257560717681133847154;
    uint256 constant betax1  = 9919335598051560070458410188669485179021901539678429405305795662259683957056;
    uint256 constant betax2  = 18205434752310456736085787745407645342708719045947011956407272318491189529635;
    uint256 constant betay1  = 5506146641172272633944310044801293120562163885353647818653626991137922625735;
    uint256 constant betay2  = 15679112657706502521207842880827832476312983613619748144255894497179600418648;
    uint256 constant gammax1 = 11559732032986387107991004021392285783925812861821192530917403151452391805634;
    uint256 constant gammax2 = 10857046999023057135944570762232829481370756359578518086990519993285655852781;
    uint256 constant gammay1 = 4082367875863433681332203403145435568316851327593401208105741076214120093531;
    uint256 constant gammay2 = 8495653923123431417604973247489272438418190587263600148770280649306958101930;
    uint256 constant deltax1 = 2928315939875135110774528473870984589490729360550238257322964084567930169487;
    uint256 constant deltax2 = 7931316076267270258024073217873143196469710802508721870829020822521936650510;
    uint256 constant deltay1 = 16544489265367506676027050412616199823309993403436408199931959020249528024960;
    uint256 constant deltay2 = 4329938055849505503749249109434700947043485017651689421449244605430110566844;

    
    uint256 constant IC0x = 2264955611172275686350057877629473177980986306184269658028330631368954461640;
    uint256 constant IC0y = 13246571585146762936134938734926603179851834353678653636339923052330520811597;
    
    uint256 constant IC1x = 20217749121022944631939229930884799774852802608287734404133287170394350099497;
    uint256 constant IC1y = 2794652679304526894193257207183576123751261539551766110212645276888963558720;
    
    uint256 constant IC2x = 20203908907832152365157145085123066367329868861192139061908877001931792376052;
    uint256 constant IC2y = 21093411002171764056709574593489098373066696806537736588397247337094433786299;
    
    uint256 constant IC3x = 1421498857696434056374447912987363521653041701034469167728603548886943899594;
    uint256 constant IC3y = 7062478251419357219217901632915795776653268065056683060808950191503209721903;
    
    uint256 constant IC4x = 3961684938546216890800140304846668790256805380878963513783070974336250740943;
    uint256 constant IC4y = 16549965640758923728090682359405777256054669861543839540324819660100792666437;
    
    uint256 constant IC5x = 18850702574828015850228403474137135049746976143629224009381523774117716150983;
    uint256 constant IC5y = 9279292293503622794674280189101533685272652988260619787848661058868598052890;
    
    uint256 constant IC6x = 9455337201847949994472210229666555495963677743641807846361304509924457827790;
    uint256 constant IC6y = 10852048672080082784543246600677771077380271836257262081439069538359638866659;
    
    uint256 constant IC7x = 13115484347592686996396289916923645541532347077778855514437915016777769179400;
    uint256 constant IC7y = 7531438775881921346297235711726657656301781692254437298530446837908763202400;
    
    uint256 constant IC8x = 2904296864025484009407754955434169768457323197319054424713946077006179145324;
    uint256 constant IC8y = 7037205530192878672844138364648998295257552033329578237621958122609610916842;
    
    uint256 constant IC9x = 2994295485045673638865244654377677962661724780937807257102032050959588332875;
    uint256 constant IC9y = 16741793597804907431695183478596031099746242413660326973520010057809832842678;
    
    uint256 constant IC10x = 7530902633110549116110526313055076239896993461124659745587803818320112732755;
    uint256 constant IC10y = 18989333593052503840918436016512514643310321529848633061439361003284741905554;
    
    uint256 constant IC11x = 17045125943217659543202288840021007249118332381143120608013694727958880941718;
    uint256 constant IC11y = 9353024977543723622903771699291742385135269778360629661975673314890880989917;
    
    uint256 constant IC12x = 8921997832207982834274283831667007677077754042816426703663663581877021540230;
    uint256 constant IC12y = 3016170883107293600265000841283253526610189131276655615051096355256993514273;
    
    uint256 constant IC13x = 11092334480975735798383674925461749864630323876413670706606426766393662019924;
    uint256 constant IC13y = 14213591943032790165097640702434937763524721542984609828419080084877165944760;
    
    uint256 constant IC14x = 7185048724573695985906531323281054030228303594203009938672474206958705329963;
    uint256 constant IC14y = 17274216621679702467405745044509383003005226050966932073465334927305430816943;
    
 
    // Memory data
    uint16 constant pVk = 0;
    uint16 constant pPairing = 128;

    uint16 constant pLastMem = 896;

    function verifyProof(uint[2] calldata _pA, uint[2][2] calldata _pB, uint[2] calldata _pC, uint[14] calldata _pubSignals) public view returns (bool) {
        assembly {
            function checkField(v) {
                if iszero(lt(v, r)) {
                    mstore(0, 0)
                    return(0, 0x20)
                }
            }
            
            // G1 function to multiply a G1 value(x,y) to value in an address
            function g1_mulAccC(pR, x, y, s) {
                let success
                let mIn := mload(0x40)
                mstore(mIn, x)
                mstore(add(mIn, 32), y)
                mstore(add(mIn, 64), s)

                success := staticcall(sub(gas(), 2000), 7, mIn, 96, mIn, 64)

                if iszero(success) {
                    mstore(0, 0)
                    return(0, 0x20)
                }

                mstore(add(mIn, 64), mload(pR))
                mstore(add(mIn, 96), mload(add(pR, 32)))

                success := staticcall(sub(gas(), 2000), 6, mIn, 128, pR, 64)

                if iszero(success) {
                    mstore(0, 0)
                    return(0, 0x20)
                }
            }

            function checkPairing(pA, pB, pC, pubSignals, pMem) -> isOk {
                let _pPairing := add(pMem, pPairing)
                let _pVk := add(pMem, pVk)

                mstore(_pVk, IC0x)
                mstore(add(_pVk, 32), IC0y)

                // Compute the linear combination vk_x
                
                g1_mulAccC(_pVk, IC1x, IC1y, calldataload(add(pubSignals, 0)))
                
                g1_mulAccC(_pVk, IC2x, IC2y, calldataload(add(pubSignals, 32)))
                
                g1_mulAccC(_pVk, IC3x, IC3y, calldataload(add(pubSignals, 64)))
                
                g1_mulAccC(_pVk, IC4x, IC4y, calldataload(add(pubSignals, 96)))
                
                g1_mulAccC(_pVk, IC5x, IC5y, calldataload(add(pubSignals, 128)))
                
                g1_mulAccC(_pVk, IC6x, IC6y, calldataload(add(pubSignals, 160)))
                
                g1_mulAccC(_pVk, IC7x, IC7y, calldataload(add(pubSignals, 192)))
                
                g1_mulAccC(_pVk, IC8x, IC8y, calldataload(add(pubSignals, 224)))
                
                g1_mulAccC(_pVk, IC9x, IC9y, calldataload(add(pubSignals, 256)))
                
                g1_mulAccC(_pVk, IC10x, IC10y, calldataload(add(pubSignals, 288)))
                
                g1_mulAccC(_pVk, IC11x, IC11y, calldataload(add(pubSignals, 320)))
                
                g1_mulAccC(_pVk, IC12x, IC12y, calldataload(add(pubSignals, 352)))
                
                g1_mulAccC(_pVk, IC13x, IC13y, calldataload(add(pubSignals, 384)))
                
                g1_mulAccC(_pVk, IC14x, IC14y, calldataload(add(pubSignals, 416)))
                

                // -A
                mstore(_pPairing, calldataload(pA))
                mstore(add(_pPairing, 32), mod(sub(q, calldataload(add(pA, 32))), q))

                // B
                mstore(add(_pPairing, 64), calldataload(pB))
                mstore(add(_pPairing, 96), calldataload(add(pB, 32)))
                mstore(add(_pPairing, 128), calldataload(add(pB, 64)))
                mstore(add(_pPairing, 160), calldataload(add(pB, 96)))

                // alpha1
                mstore(add(_pPairing, 192), alphax)
                mstore(add(_pPairing, 224), alphay)

                // beta2
                mstore(add(_pPairing, 256), betax1)
                mstore(add(_pPairing, 288), betax2)
                mstore(add(_pPairing, 320), betay1)
                mstore(add(_pPairing, 352), betay2)

                // vk_x
                mstore(add(_pPairing, 384), mload(add(pMem, pVk)))
                mstore(add(_pPairing, 416), mload(add(pMem, add(pVk, 32))))


                // gamma2
                mstore(add(_pPairing, 448), gammax1)
                mstore(add(_pPairing, 480), gammax2)
                mstore(add(_pPairing, 512), gammay1)
                mstore(add(_pPairing, 544), gammay2)

                // C
                mstore(add(_pPairing, 576), calldataload(pC))
                mstore(add(_pPairing, 608), calldataload(add(pC, 32)))

                // delta2
                mstore(add(_pPairing, 640), deltax1)
                mstore(add(_pPairing, 672), deltax2)
                mstore(add(_pPairing, 704), deltay1)
                mstore(add(_pPairing, 736), deltay2)


                let success := staticcall(sub(gas(), 2000), 8, _pPairing, 768, _pPairing, 0x20)

                isOk := and(success, mload(_pPairing))
            }

            let pMem := mload(0x40)
            mstore(0x40, add(pMem, pLastMem))

            // Validate that all evaluations ∈ F
            
            checkField(calldataload(add(_pubSignals, 0)))
            
            checkField(calldataload(add(_pubSignals, 32)))
            
            checkField(calldataload(add(_pubSignals, 64)))
            
            checkField(calldataload(add(_pubSignals, 96)))
            
            checkField(calldataload(add(_pubSignals, 128)))
            
            checkField(calldataload(add(_pubSignals, 160)))
            
            checkField(calldataload(add(_pubSignals, 192)))
            
            checkField(calldataload(add(_pubSignals, 224)))
            
            checkField(calldataload(add(_pubSignals, 256)))
            
            checkField(calldataload(add(_pubSignals, 288)))
            
            checkField(calldataload(add(_pubSignals, 320)))
            
            checkField(calldataload(add(_pubSignals, 352)))
            
            checkField(calldataload(add(_pubSignals, 384)))
            
            checkField(calldataload(add(_pubSignals, 416)))
            

            // Validate all evaluations
            let isValid := checkPairing(_pA, _pB, _pC, _pubSignals, pMem)

            mstore(0, isValid)
             return(0, 0x20)
         }
     }
 }
